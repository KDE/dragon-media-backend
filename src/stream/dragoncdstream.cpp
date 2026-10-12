/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncdstream.h"

#include "dragonbufferprogress.h"
#include "dragoncddisc.h"
#include "dragonmediabackend_network_logging.h"

#include <KLocalizedString>
#include <QUrlQuery>

#include <algorithm>
#include <ranges>
#include <span>

using namespace Qt::StringLiterals;

namespace
{

static_assert(DragonCdStream::SECTOR_SIZE == DragonCdDisc::SECTOR_SIZE);
static_assert(DragonCdStream::READ_BLOCK_SECTORS == DragonCdDisc::READ_BLOCK_SECTORS);

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
    m_disc.reset();
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

    auto disc = DragonCdDisc::acquire(device);
    if (!disc) {
        fail(disc.error());
        return;
    }
    m_disc = std::move(*disc);

    const int firstTrack = m_disc->firstTrack();
    const int lastTrack = m_disc->lastTrack();

    int trackNum = -1;
    if (haveTrackOverride) {
        trackNum = trackOverride;
    } else {
        trackNum = trackNumberFromName(fileName, firstTrack, lastTrack).value_or(-1);
    }
    const DragonCdDisc::Track *track = m_disc->track(trackNum);
    if (!track) {
        fail(i18n("Track not found on the CD: %1", fileName));
        return;
    }

    if (!track->isAudio) {
        fail(i18n("CD track %1 is not an audio track.", trackNum));
        return;
    }

    if (track->lastLsn < track->firstLsn) {
        fail(i18n("Could not determine the length of CD track %1.", trackNum));
        return;
    }

    m_firstLsn = track->firstLsn;
    m_lastLsn = track->lastLsn;
    const qint64 dataBytes = (m_lastLsn - m_firstLsn + 1) * SECTOR_SIZE;

    m_wavHeader = makeWavHeader(uint32_t(dataBytes));

    m_totalSize = WAV_HEADER_SIZE + dataBytes;

    qCDebug(dragonMediaBackendNetwork) << "DragonCdStream track" << trackNum << "lsn" << m_firstLsn << "to" << m_lastLsn << "bytes" << dataBytes;

    Q_EMIT seekableChanged(true);
    m_bufferProgress->setDecoderReady(true);
    Q_EMIT streamBuffered();
}

void DragonCdStream::stop()
{
    m_abort = true;

    std::scoped_lock lock(m_ioMutex);
    m_disc.reset();
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

    if (m_abort.load() || m_error.load() || !m_disc) {
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
    if (!m_disc) {
        return false;
    }

    const int64_t remaining = m_lastLsn - lsn + 1;
    if (remaining <= 0) {
        return false;
    }

    const uint32_t sectors = m_disc->readAudioSectors(m_blockCache, lsn, uint32_t(std::min<int64_t>(READ_BLOCK_SECTORS, remaining)));
    if (sectors == 0) {
        qCWarning(dragonMediaBackendNetwork) << "CD read error at lsn" << lsn;
        m_disc->invalidate();
        fail(i18n("Error reading the CD. The disc may have been ejected."));
        return false;
    }

    m_cacheStartLsn = lsn;
    m_cacheSectors = int32_t(sectors);
    return true;
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
    m_disc.reset();
    qCWarning(dragonMediaBackendNetwork) << "DragonCdStream:" << message;
    Q_EMIT errorOccurred(message);
}
