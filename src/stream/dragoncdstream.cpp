/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncdstream.h"

#include "dragonbufferprogress.h"
#include "dragonmediabackend_network_logging.h"

#include <KLocalizedString>
#include <QUrlQuery>

#include <cdio++/cdio.hpp>
#include <cdio/disc.h>
#include <cdio/track.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <span>

using namespace Qt::StringLiterals;

namespace
{

constexpr int SAMPLES_PER_SECTOR = DragonCdStream::SECTOR_SIZE / 2; // 16-bit samples
constexpr int ENDIANNESS_PROBE_SECTORS = 10;

#pragma pack(push, 1)
struct WavHeader {
    std::array<char, 4> riff = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    std::array<char, 4> wave = {'W', 'A', 'V', 'E'};
    std::array<char, 4> fmt = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 2; // Stereo
    uint32_t sampleRate = 44100;
    uint32_t byteRate = 44100 * 2 * 2;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    std::array<char, 4> data = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == DragonCdStream::WAV_HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<WavHeader>);

void swapLeBe16(std::span<uint8_t> data)
{
    auto samples = std::span{reinterpret_cast<uint16_t *>(data.data()), data.size() / 2};
    for (auto &sample : samples) {
        sample = std::byteswap(sample);
    }
}

}

DragonCdStream::DragonCdStream(QObject *parent)
    : DragonStream(parent)
    , m_bufferProgress(new DragonBufferProgress(this))
    , m_blockCache(static_cast<qsizetype>(READ_BLOCK_SECTORS) * SECTOR_SIZE)
{
}

DragonCdStream::~DragonCdStream()
{
    DragonCdStream::stop();
}

void DragonCdStream::setUrl(const QUrl &url)
{
    m_url = url;
}

std::array<uint8_t, 44> DragonCdStream::makeWavHeader(uint32_t dataSize)
{
    const WavHeader hdr{
        .fileSize = 36 + dataSize,
        .dataSize = dataSize,
    };
    return std::bit_cast<std::array<uint8_t, 44>>(hdr);
}

std::optional<int> DragonCdStream::trackNumberFromName(const QString &fileName, int firstTrackNum, int lastTrackNum)
{
    QString name = fileName;
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot >= 0) {
        name.truncate(dot);
    }

    struct Run {
        int value = 0;
        qsizetype length = 0;
    };
    std::vector<Run> runs;

    qsizetype i = 0;
    while (i < name.size()) {
        if (!name[i].isDigit()) {
            ++i;
            continue;
        }
        qsizetype start = i;
        while (i < name.size() && name[i].isDigit()) {
            ++i;
        }
        bool ok = false;
        const int value = name.mid(start, i - start).toInt(&ok);
        if (ok) {
            runs.push_back(Run{value, i - start});
        }
    }

    auto validRuns = runs | std::views::filter([=](const Run &r) {
                         return r.value >= firstTrackNum && r.value <= lastTrackNum;
                     });

    if (auto it = std::ranges::find_if(validRuns,
                                       [](const Run &r) {
                                           return r.length == 2;
                                       });
        it != std::ranges::end(validRuns)) {
        return it->value;
    }
    if (auto it = std::ranges::begin(validRuns); it != std::ranges::end(validRuns)) {
        return it->value;
    }
    return std::nullopt;
}

void DragonCdStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "DragonCdStream start" << m_url.toString();

    std::scoped_lock lock(m_ioMutex);

    m_abort = false;
    m_error = false;
    m_streamPosition = 0;
    m_totalSize = -1;
    m_cacheSectors = 0;
    m_cacheStartLsn = -1;
    m_readSectorsLimit = READ_BLOCK_SECTORS;
    m_swapLeBe = false;
    m_device.reset();
    m_bufferProgress->reset();

    const QString fileName = m_url.fileName();
    const QUrlQuery query(m_url);
    const QString device = query.hasQueryItem(u"device"_s) ? query.queryItemValue(u"device"_s, QUrl::FullyDecoded) : QString();

    int trackOverride = -1;
    bool haveTrackOverride = false;
    if (query.hasQueryItem(u"track"_s)) {
        bool ok = false;
        trackOverride = query.queryItemValue(u"track"_s, QUrl::FullyDecoded).toInt(&ok);
        haveTrackOverride = ok;
    }

    if (fileName.isEmpty()) {
        fail(i18n("The audiocd:/ URL does not name a track to play."));
        return;
    }

    m_device = std::make_unique<CdioDevice>();
    const QByteArray deviceBytes = device.toLocal8Bit();
    // Note: the single-argument open() maps to cdio_open_cd(), which only
    // probes OS device drivers and rejects disk images; cdio_open() with
    // DRIVER_UNKNOWN scans everything, including bin/cue images.
    if (!m_device->open(deviceBytes.isEmpty() ? nullptr : deviceBytes.constData(), DRIVER_UNKNOWN)) {
        fail(i18n("Could not open the CD device. Check that a disc is present."));
        return;
    }

    const discmode_t discmode = m_device->getDiscmode();
    if (discmode != CDIO_DISC_MODE_CD_DA && discmode != CDIO_DISC_MODE_CD_MIXED) {
        fail(i18n("The disc is not an Audio CD."));
        return;
    }

    const track_t firstTrack = m_device->getFirstTrackNum();
    const track_t numTracks = m_device->getNumTracks();
    if (firstTrack == CDIO_INVALID_TRACK || numTracks == CDIO_INVALID_TRACK || numTracks <= 0) {
        fail(i18n("The CD has no tracks."));
        return;
    }
    const int lastTrack = int(firstTrack) + int(numTracks) - 1;

    int trackNum = -1;
    if (haveTrackOverride) {
        trackNum = trackOverride;
    } else {
        trackNum = trackNumberFromName(fileName, int(firstTrack), lastTrack).value_or(-1);
    }
    if (trackNum < int(firstTrack) || trackNum > lastTrack) {
        fail(i18n("Track not found on the CD: %1", fileName));
        return;
    }

    CdioTrack track(m_device->getCdIo(), track_t(trackNum));
    if (track.getFormat() != TRACK_FORMAT_AUDIO) {
        fail(i18n("CD track %1 is not an audio track.", trackNum));
        return;
    }

    const lsn_t startLsn = track.getLsn();
    const unsigned int secCount = track.getSecCount();
    if (startLsn == CDIO_INVALID_LSN || secCount == 0) {
        fail(i18n("Could not determine the length of CD track %1.", trackNum));
        return;
    }

    m_firstLsn = startLsn;
    m_lastLsn = int64_t(startLsn) + int64_t(secCount) - 1;
    const qint64 dataBytes = (m_lastLsn - m_firstLsn + 1) * SECTOR_SIZE;

    m_wavHeader = makeWavHeader(uint32_t(dataBytes));
    detectEndianness();

    m_totalSize = WAV_HEADER_SIZE + dataBytes;

    qCDebug(dragonMediaBackendNetwork) << "DragonCdStream track" << trackNum << "lsn" << m_firstLsn << "to" << m_lastLsn << "bytes" << dataBytes << "swapLeBe"
                                       << m_swapLeBe;

    Q_EMIT seekableChanged(true);
    m_bufferProgress->setDecoderReady(true);
    Q_EMIT streamBuffered();
}

void DragonCdStream::stop()
{
    m_abort = true;

    std::scoped_lock lock(m_ioMutex);
    if (m_device) {
        m_device->close();
        m_device.reset();
    }
    m_cacheSectors = 0;
    m_cacheStartLsn = -1;
}

int DragonCdStream::read(std::span<uint8_t> buf, std::stop_token st)
{
    if (buf.empty()) {
        return 0;
    }

    if (st.stop_requested()) {
        return -1;
    }

    std::scoped_lock lock(m_ioMutex);

    if (m_abort.load() || m_error.load() || !m_device) {
        return -1;
    }

    const qint64 total = m_totalSize.load();
    if (total <= 0) {
        return -1;
    }

    qint64 pos = m_streamPosition.load();
    if (pos >= total) {
        return 0;
    }

    qint64 copied = 0;
    while (copied < qint64(buf.size()) && pos + copied < total) {
        if (pos + copied < WAV_HEADER_SIZE) {
            const auto available = std::min<qint64>({WAV_HEADER_SIZE - (pos + copied), static_cast<qint64>(buf.size()) - copied, total - (pos + copied)});
            std::copy_n(m_wavHeader.begin() + (pos + copied), available, buf.begin() + copied);
            copied += available;
            continue;
        }

        const qint64 dataPos = pos + copied - WAV_HEADER_SIZE;
        const int64_t sectorIndex = dataPos / SECTOR_SIZE;
        const int64_t wantedLsn = m_firstLsn + sectorIndex;

        if (m_cacheSectors == 0 || wantedLsn < m_cacheStartLsn || wantedLsn >= m_cacheStartLsn + m_cacheSectors) {
            if (st.stop_requested()) {
                break;
            }
            if (!fillCache(wantedLsn)) {
                break;
            }
        }

        const qint64 cacheOffset = (wantedLsn - m_cacheStartLsn) * SECTOR_SIZE + (dataPos % SECTOR_SIZE);
        const qint64 available = std::min<qint64>({qint64(m_cacheSectors) * SECTOR_SIZE - cacheOffset, qint64(buf.size()) - copied, total - (pos + copied)});
        std::copy_n(m_blockCache.begin() + cacheOffset, available, buf.begin() + copied);
        copied += available;
    }

    if (copied == 0) {
        return st.stop_requested() ? -1 : (m_error.load() ? -1 : 0);
    }

    m_streamPosition.store(pos + copied);
    return int(copied);
}

bool DragonCdStream::fillCache(int64_t lsn)
{
    if (!m_device) {
        return false;
    }

    const int64_t remaining = m_lastLsn - lsn + 1;
    if (remaining <= 0) {
        return false;
    }

    uint32_t sectors = uint32_t(std::min<int64_t>(m_readSectorsLimit, remaining));
    for (;;) {
        try {
            m_device->readSectors(m_blockCache.data(), lsn_t(lsn), CDIO_READ_MODE_AUDIO, sectors);
        } catch (DriverOpException &e) {
            if (sectors > 1) {
                m_readSectorsLimit = int32_t(sectors / 2);
                qCDebug(dragonMediaBackendNetwork) << "CD read of" << sectors << "sectors at lsn" << lsn << "failed:" << e.get_msg() << "- retrying with"
                                                   << m_readSectorsLimit << "sectors";
                sectors = uint32_t(m_readSectorsLimit);
                continue;
            }
            qCWarning(dragonMediaBackendNetwork) << "CD read error at lsn" << lsn << ":" << e.get_msg();
            fail(i18n("Error reading the CD. The disc may have been ejected."));
            return false;
        }
        break;
    }

    if (m_swapLeBe) {
        swapLeBe16(std::span{m_blockCache}.subspan(0, static_cast<size_t>(sectors) * SECTOR_SIZE));
    }

    m_cacheStartLsn = lsn;
    m_cacheSectors = int32_t(sectors);
    return true;
}

void DragonCdStream::detectEndianness()
{
    m_swapLeBe = false;

    const int64_t trackSectors = m_lastLsn - m_firstLsn + 1;
    if (trackSectors < 2) {
        return;
    }

    std::vector<uint8_t> sector(SECTOR_SIZE);
    constexpr double positions[] = {0.5, 0.25, 0.75};
    for (const double fraction : positions) {
        int64_t from = m_firstLsn + int64_t(double(trackSectors) * fraction);
        int64_t to = std::min<int64_t>(from + ENDIANNESS_PROBE_SECTORS, m_lastLsn + 1);
        from = std::max<int64_t>(from, m_firstLsn);
        if (from >= to) {
            continue;
        }

        double nativeSum0 = 0.0, nativeSum1 = 0.0;
        double swappedSum0 = 0.0, swappedSum1 = 0.0;

        for (int64_t lsn = from; lsn < to; ++lsn) {
            try {
                m_device->readSectors(sector.data(), lsn_t(lsn), CDIO_READ_MODE_AUDIO, 1);
            } catch (DriverOpException &) {
                return; // inconclusive, assume host endianness
            }
            const auto pcm = std::span{reinterpret_cast<const uint16_t *>(sector.data()), static_cast<size_t>(SAMPLES_PER_SECTOR)};
            uint16_t lastNative = 0, lastSwapped = 0;
            for (const uint16_t native : pcm | std::views::stride(2)) {
                const uint16_t swapped = std::byteswap(native);
                nativeSum0 += std::abs(static_cast<int16_t>(native));
                nativeSum1 += std::abs(static_cast<int16_t>(native) - static_cast<int16_t>(lastNative));
                lastNative = native;
                swappedSum0 += std::abs(static_cast<int16_t>(swapped));
                swappedSum1 += std::abs(static_cast<int16_t>(swapped) - static_cast<int16_t>(lastSwapped));
                lastSwapped = swapped;
            }
        }

        if (nativeSum0 <= 0.0 || swappedSum0 <= 0.0) {
            continue; // silence, inconclusive
        }
        const double nativeFactor = nativeSum1 / nativeSum0;
        const double swappedFactor = swappedSum1 / swappedSum0;
        const double diff = nativeFactor - swappedFactor;
        if (diff > 0.5) {
            m_swapLeBe = true;
            qCDebug(dragonMediaBackendNetwork) << "Drive produces CD samples in other endianness; swapping";
            return;
        }
        if (diff < -0.5) {
            qCDebug(dragonMediaBackendNetwork) << "Drive produces CD samples in host endianness";
            return;
        }
    }
}

qint64 DragonCdStream::seek(qint64 offset)
{
    std::scoped_lock lock(m_ioMutex);

    const qint64 total = m_totalSize.load();
    if (total <= 0 || m_error.load() || m_abort.load()) {
        return -1;
    }
    if (offset < 0) {
        return -1;
    }
    if (offset > total) {
        offset = total;
    }

    m_streamPosition.store(offset);
    return offset;
}

qint64 DragonCdStream::size() const
{
    return m_totalSize.load();
}

qint64 DragonCdStream::position() const
{
    return m_streamPosition.load();
}

DragonBufferProgress *DragonCdStream::bufferProgress() const
{
    return m_bufferProgress;
}

void DragonCdStream::fail(const QString &message)
{
    m_error = true;
    if (m_device) {
        m_device->close();
        m_device.reset();
    }
    qCWarning(dragonMediaBackendNetwork) << "DragonCdStream:" << message;
    Q_EMIT errorOccurred(message);
}
