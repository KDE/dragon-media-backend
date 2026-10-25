/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncdstream.h"

#include "dragoncddisc.h"
#include "dragoncdurl.h"
#include "dragonmediabackend_network_logging.h"
#include "stream/dragonbufferprogress.h"

#include <KLocalizedString>
#include <QtEndian>

#include <cstddef>
#include <span>
#include <string_view>

using namespace Qt::StringLiterals;

namespace
{

#pragma pack(push, 1)
struct WavHeader {
    std::array<char, 4> riff = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    std::array<char, 4> wave = {'W', 'A', 'V', 'E'};
    std::array<char, 4> fmt = {'f', 'm', 't', ' '};
    uint32_t fmtSize = qToLittleEndian(16u);
    uint16_t audioFormat = qToLittleEndian(uint16_t{1}); // PCM
    uint16_t numChannels = qToLittleEndian(uint16_t{2}); // Stereo
    uint32_t sampleRate = qToLittleEndian(44100u);
    uint32_t byteRate = qToLittleEndian(44100u * 2 * 2);
    uint16_t blockAlign = qToLittleEndian(uint16_t{4});
    uint16_t bitsPerSample = qToLittleEndian(uint16_t{16});
    std::array<char, 4> data = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == DragonCdStream::WAV_HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<WavHeader>);

constexpr uint16_t le16(const std::array<uint8_t, 44> &bytes, size_t offset)
{
    return uint16_t(bytes[offset]) | (uint16_t(bytes[offset + 1]) << 8);
}

constexpr uint32_t le32(const std::array<uint8_t, 44> &bytes, size_t offset)
{
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) | (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}

constexpr uint32_t fourcc(std::string_view tag)
{
    return uint32_t(uint8_t(tag[0])) | (uint32_t(uint8_t(tag[1])) << 8) | (uint32_t(uint8_t(tag[2])) << 16) | (uint32_t(uint8_t(tag[3])) << 24);
}

// check the bit cast structure
constexpr auto kDefaultWavHeader = std::bit_cast<std::array<uint8_t, 44>>(WavHeader{});
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, riff)) == fourcc("RIFF"));
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, wave)) == fourcc("WAVE"));
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, fmt)) == fourcc("fmt "));
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, data)) == fourcc("data"));
static_assert(le16(kDefaultWavHeader, offsetof(WavHeader, audioFormat)) == 1); // PCM
static_assert(le16(kDefaultWavHeader, offsetof(WavHeader, numChannels)) == 2); // stereo
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, sampleRate)) == 44100);
static_assert(le32(kDefaultWavHeader, offsetof(WavHeader, byteRate)) == 44100 * 2 * 2); // sampleRate * channels * 2 bytes
static_assert(le16(kDefaultWavHeader, offsetof(WavHeader, blockAlign)) == 4); // channels * 2 bytes
static_assert(le16(kDefaultWavHeader, offsetof(WavHeader, bitsPerSample)) == 16);
}

DragonCdStream::DragonCdStream(QObject *parent)
    : DragonStream(parent)
    , m_bufferProgress(new DragonBufferProgress(this))
    , m_blockCache(static_cast<qsizetype>(DragonCdDisc::READ_BLOCK_SECTORS) * DragonCdDisc::SECTOR_SIZE)
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
        .fileSize = qToLittleEndian(36u + dataSize),
        .dataSize = qToLittleEndian(dataSize),
    };
    return std::bit_cast<std::array<uint8_t, 44>>(hdr);
}

void DragonCdStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "DragonCdStream start" << m_url.toString();

    {
        std::scoped_lock lock(m_ioMutex);
        m_abort = false;
        m_error = false;
        m_streamPosition = 0;
        m_totalSize = -1;
        m_cacheSectors = 0;
        m_cacheStartLogicalSector = -1;
        m_disc.reset();
        m_bufferProgress->reset();
    }

    if (m_url.fileName().isEmpty()) {
        const QString message = i18n("The audiocd:/ URL does not name a track to play.");
        fail(message);
        Q_EMIT errorOccurred(message);
        return;
    }

    // Open the device without holding m_ioMutex: spinning up the drive can
    // take seconds, and stop() must stay responsive meanwhile.
    auto disc = DragonCdDisc::acquire(DragonCdUrl::deviceFromUrl(m_url));
    if (!disc) {
        fail(disc.error());
        Q_EMIT errorOccurred(disc.error());
        return;
    }

    // The failure message travels out of the locked section so that
    // errorOccurred is emitted with the lock released.
    const std::optional<QString> failure = [&]() -> std::optional<QString> {
        std::scoped_lock lock(m_ioMutex);

        const auto failLocked = [&](QString message) {
            fail(message);
            return std::optional<QString>(std::move(message));
        };

        if (m_abort.load()) {
            // stop() ran while the drive was spinning up; drop the handle.
            return std::nullopt;
        }

        m_disc = std::move(*disc);

        const int firstTrack = m_disc->firstTrack();
        const int lastTrack = m_disc->lastTrack();

        int trackNum = DragonCdUrl::resolveTrackNumber(m_url, firstTrack, lastTrack).value_or(-1);

        if (trackNum < firstTrack || trackNum > lastTrack) {
            return failLocked(i18n("Track not found on the CD: %1", m_url.fileName()));
        }

        if (!m_disc->isAudioTrack(trackNum)) {
            return failLocked(i18n("CD track %1 is not an audio track.", trackNum));
        }

        const auto [firstLogicalSector, lastLogicalSector] = m_disc->trackLogicalSectorRange(trackNum);
        m_firstLogicalSector = firstLogicalSector;
        m_lastLogicalSector = lastLogicalSector;
        if (m_lastLogicalSector < m_firstLogicalSector) {
            return failLocked(i18n("Could not determine the length of CD track %1.", trackNum));
        }

        const qint64 dataBytes = (m_lastLogicalSector - m_firstLogicalSector + 1) * DragonCdDisc::SECTOR_SIZE;

        m_wavHeader = makeWavHeader(uint32_t(dataBytes));

        m_totalSize = WAV_HEADER_SIZE + dataBytes;

        qCDebug(dragonMediaBackendNetwork) << "DragonCdStream track" << trackNum << "logical sector" << m_firstLogicalSector << "to" << m_lastLogicalSector
                                           << "bytes" << dataBytes;

        return std::nullopt;
    }();

    if (m_abort.load()) {
        return;
    }

    if (failure) {
        Q_EMIT errorOccurred(*failure);
        return;
    }

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
    m_cacheStartLogicalSector = -1;
}

int DragonCdStream::read(std::span<uint8_t> buf, const std::stop_token &st)
{
    if (buf.empty()) {
        return 0;
    }

    if (st.stop_requested()) {
        return -1;
    }

    const auto result = readLocked(buf, st);

    if (!result.has_value()) {
        Q_EMIT errorOccurred(result.error());
        return -1;
    }

    return *result;
}

std::expected<int, QString> DragonCdStream::readLocked(std::span<uint8_t> buf, const std::stop_token &st)
{
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
        const qint64 sectorIndex = dataPos / DragonCdDisc::SECTOR_SIZE;
        const qint64 wantedLogicalSector = m_firstLogicalSector + sectorIndex;

        if (m_cacheSectors == 0 || wantedLogicalSector < m_cacheStartLogicalSector || wantedLogicalSector >= m_cacheStartLogicalSector + m_cacheSectors) {
            if (st.stop_requested()) {
                break;
            }
            const auto fill = fillCache(wantedLogicalSector);
            if (!fill.has_value()) {
                return std::unexpected(fill.error());
            }
        }

        const qint64 cacheOffset = (wantedLogicalSector - m_cacheStartLogicalSector) * DragonCdDisc::SECTOR_SIZE + (dataPos % DragonCdDisc::SECTOR_SIZE);
        const qint64 available =
            std::min<qint64>({qint64(m_cacheSectors) * DragonCdDisc::SECTOR_SIZE - cacheOffset, qint64(buf.size()) - copied, total - (pos + copied)});
        std::copy_n(m_blockCache.begin() + cacheOffset, available, buf.begin() + copied);
        copied += available;
    }

    if (copied == 0) {
        return st.stop_requested() ? -1 : (m_error.load() ? -1 : 0);
    }

    m_streamPosition.store(pos + copied);
    return int(copied);
}

std::expected<void, QString> DragonCdStream::fillCache(qint64 startLogicalSector)
{
    if (!m_disc) {
        return std::unexpected(i18n("The CD is no longer available."));
    }

    const qint64 remaining = m_lastLogicalSector - startLogicalSector + 1;
    if (remaining <= 0) {
        return std::unexpected(i18n("Attempted to read past the end of the CD track."));
    }

    const uint32_t sectors =
        m_disc->readAudioSectors(m_blockCache, startLogicalSector, uint32_t(std::min<qint64>(DragonCdDisc::READ_BLOCK_SECTORS, remaining)));
    if (sectors == 0) {
        qCWarning(dragonMediaBackendNetwork) << "CD read error at logical sector" << startLogicalSector;
        m_disc->invalidate();
        const QString message = i18n("Error reading the CD. The disc may have been ejected.");
        fail(message);
        return std::unexpected(message);
    }

    m_cacheStartLogicalSector = startLogicalSector;
    m_cacheSectors = int32_t(sectors);
    return {};
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
}
