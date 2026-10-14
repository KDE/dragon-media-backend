/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncddisc.h"

#include "dragoncdframes.h"
#include "dragoncdtoc_p.h"
#include "dragoncdurl.h"

#include "dragonmediabackend_network_logging.h"

#include <KLocalizedString>
#include <QFileInfo>

#include <cdio/cdio.h>
#include <cdio/disc.h>
#include <cdio/memory.h>
#include <cdio/read.h>
#include <cdio/track.h>

#include <algorithm>
#include <map>
#include <memory>
#include <ranges>
#include <vector>

// our own RAII container instead of using cdio++'s CdioDevice
class CdIoHandle
{
public:
    explicit CdIoHandle(const char *source)
        : m_cdio(cdio_open(source, DRIVER_UNKNOWN))
    {
    }

    ~CdIoHandle()
    {
        cdio_destroy(m_cdio);
    }

    CdIoHandle(const CdIoHandle &) = delete;
    CdIoHandle &operator=(const CdIoHandle &) = delete;

    [[nodiscard]] CdIo_t *get()
    {
        return m_cdio;
    }

    [[nodiscard]] explicit operator bool() const
    {
        return m_cdio != nullptr;
    }

private:
    CdIo_t *m_cdio = nullptr;
};

namespace
{

std::mutex s_acquireMutex;
std::map<QString, std::weak_ptr<DragonCdDisc>> s_cache;

QString resolveDevice(const QString &device)
{
    QString resolved = device;
    if (resolved.isEmpty()) {
        // libcdio allocates the string with its own allocator, so free it
        // through cdio_free rather than std::free.
        const std::unique_ptr<char, decltype(&cdio_free)> defaultDevice(cdio_get_default_device(nullptr), &cdio_free);
        if (defaultDevice) {
            resolved = QString::fromLocal8Bit(defaultDevice.get());
        }
    }
    // Fold symlinks such as /dev/cdrom -> /dev/sr0 and relative image paths
    // so every spelling of the same drive shares one handle.
    const QFileInfo info(resolved);
    if (!resolved.isEmpty() && info.exists()) {
        resolved = info.canonicalFilePath();
    }
    return resolved;
}

std::unique_ptr<CdIoHandle> openDevice(const QString &device)
{
    const QByteArray deviceBytes = device.toLocal8Bit();
    auto cdio = std::make_unique<CdIoHandle>(deviceBytes.isEmpty() ? nullptr : deviceBytes.constData());
    if (!*cdio) {
        return nullptr;
    }
    return cdio;
}

}

std::expected<DragonCdToc, QString> DragonCdDisc::readToc(CdIoHandle &cdioDevice, const QString &device)
{
    CdIo_t *const cdio = cdioDevice.get();

    const discmode_t discmode = cdio_get_discmode(cdio);
    if (discmode != CDIO_DISC_MODE_CD_DA && discmode != CDIO_DISC_MODE_CD_MIXED) {
        return std::unexpected(i18n("The disc is not an Audio CD."));
    }

    const track_t firstTrack = cdio_get_first_track_num(cdio);
    const track_t numTracks = cdio_get_num_tracks(cdio);
    if (firstTrack == CDIO_INVALID_TRACK || numTracks == CDIO_INVALID_TRACK || numTracks <= 0) {
        return std::unexpected(i18n("The CD has no tracks."));
    }

    const int first = int(firstTrack);
    const int last = first + int(numTracks) - 1;
    const int64_t leadOutLogicalSector = cdio_get_track_lsn(cdio, CDIO_CDROM_LEADOUT_TRACK);

    auto tocPrivate = std::make_unique<DragonCdTocPrivate>();
    tocPrivate->device = device;
    tocPrivate->leadOutFrame = leadOutLogicalSector >= 0 ? int(leadOutLogicalSector) + DragonCdFrames::kLeadInFrames : 0;

    for (int number = first; number <= last; ++number) {
        const int64_t startLogicalSector = cdio_get_track_lsn(cdio, track_t(number));
        const int64_t endLogicalSector = (number < last) ? cdio_get_track_lsn(cdio, track_t(number + 1)) : leadOutLogicalSector;
        if (startLogicalSector < 0 || endLogicalSector < startLogicalSector) {
            continue;
        }

        const bool isAudio = cdio_get_track_format(cdio, track_t(number)) == TRACK_FORMAT_AUDIO;

        auto trackPrivate = std::make_unique<DragonCdTrackPrivate>();
        trackPrivate->number = number;
        trackPrivate->isAudio = isAudio;
        trackPrivate->startFrame = int(startLogicalSector) + DragonCdFrames::kLeadInFrames;
        trackPrivate->lengthFrames = int(endLogicalSector - startLogicalSector);
        if (isAudio) {
            trackPrivate->url = DragonCdUrl::makeTrackUrl(number, device);
        }
        tocPrivate->tracks.append(DragonCdTrack(trackPrivate.release()));
    }

    qCDebug(dragonMediaBackendNetwork) << "DragonCdToc built:" << tocPrivate->device << "tracks" << tocPrivate->tracks.size() << "leadOutFrame"
                                       << tocPrivate->leadOutFrame;
    return DragonCdToc(tocPrivate.release());
}

std::expected<std::shared_ptr<DragonCdDisc>, QString> DragonCdDisc::acquire(const QString &device)
{
    const QString key = resolveDevice(device);

    std::scoped_lock lock(s_acquireMutex);

    std::erase_if(s_cache, [](const auto &entry) {
        return entry.second.expired();
    });

    if (auto it = s_cache.find(key); it != s_cache.end()) {
        if (const std::shared_ptr<DragonCdDisc> disc = it->second.lock()) {
            if (disc->stillCurrent(key)) {
                return disc;
            }
        }
        s_cache.erase(it);
    }

    auto opened = open(key, key.isEmpty() ? device : key);
    if (!opened) {
        return opened;
    }

    s_cache[key] = *opened;
    return opened;
}

std::expected<std::shared_ptr<DragonCdDisc>, QString> DragonCdDisc::open(const QString &cacheKey, const QString &device)
{
    auto cdio = openDevice(device);
    if (!cdio) {
        return std::unexpected(i18n("Could not open the CD device. Check that a disc is present."));
    }

    auto toc = readToc(*cdio, device);
    if (!toc) {
        return std::unexpected(toc.error());
    }

    // cdio_get_media_changed() reports changes since the last query on the
    // device, so prime it now: the next query then only reports a change
    // that happened while this handle was open.
    cdio_get_media_changed(cdio->get());

    std::shared_ptr<DragonCdDisc> disc(new DragonCdDisc);
    disc->m_cacheKey = cacheKey;
    disc->m_toc = std::move(*toc);
    const QFileInfo info(device);
    if (info.isFile()) {
        disc->m_fileSize = info.size();
        disc->m_fileModified = info.lastModified();
    }
    disc->m_device = std::move(cdio);

    qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc opened" << cacheKey << "tracks" << disc->firstTrack() << "to" << disc->lastTrack();
    return disc;
}

bool DragonCdDisc::stillCurrent(const QString &device)
{
    if (m_invalid.load()) {
        return false;
    }

    if (const QFileInfo info(device); info.isFile()) {
        if (info.size() == m_fileSize && info.lastModified() == m_fileModified) {
            return true;
        }
        qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc image changed on" << m_cacheKey;
        return false;
    }

    int changed = 0;
    {
        std::scoped_lock lock(m_ioMutex);
        if (!m_device) {
            return false;
        }
        changed = cdio_get_media_changed(m_device->get());
    }

    if (changed == 0) {
        return true;
    }
    if (changed > 0) {
        qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc media changed on" << m_cacheKey;
        return false;
    }

    auto probe = openDevice(device);
    if (!probe) {
        return false;
    }
    const auto toc = readToc(*probe, device);
    if (!toc || !(*toc == m_toc)) {
        qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc TOC changed on" << m_cacheKey;
        return false;
    }
    return true;
}

DragonCdDisc::~DragonCdDisc()
{
    std::scoped_lock lock(m_ioMutex);
    m_device.reset();
    qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc closed" << m_cacheKey;
}

int DragonCdDisc::firstTrack() const
{
    const QList<DragonCdTrack> tracks = m_toc.tracks();
    return tracks.isEmpty() ? 0 : tracks.first().number();
}

int DragonCdDisc::lastTrack() const
{
    const QList<DragonCdTrack> tracks = m_toc.tracks();
    return tracks.isEmpty() ? -1 : tracks.last().number();
}

bool DragonCdDisc::isAudioTrack(int number) const
{
    const QList<DragonCdTrack> tracks = m_toc.tracks();
    const auto it = std::ranges::find(tracks, number, &DragonCdTrack::number);
    return it != tracks.cend() && it->isAudio();
}

std::pair<int64_t, int64_t> DragonCdDisc::trackLogicalSectorRange(int number) const
{
    const QList<DragonCdTrack> tracks = m_toc.tracks();
    const auto it = std::ranges::find(tracks, number, &DragonCdTrack::number);
    if (it == tracks.cend() || it->lengthFrames() <= 0) {
        return {-1, -1};
    }
    const int64_t firstLogicalSector = int64_t(it->startFrame()) - DragonCdFrames::kLeadInFrames;
    return {firstLogicalSector, firstLogicalSector + int64_t(it->lengthFrames()) - 1};
}

uint32_t DragonCdDisc::readAudioSectors(std::span<uint8_t> buf, int64_t startLogicalSector, uint32_t sectors)
{
    std::scoped_lock lock(m_ioMutex);

    if (!m_device || m_invalid.load()) {
        return 0;
    }

    sectors = std::min({sectors, uint32_t(m_readSectorsLimit), uint32_t(buf.size() / SECTOR_SIZE)});
    if (sectors == 0) {
        return 0;
    }
    for (;;) {
        const driver_return_code_t drc = cdio_read_sectors(m_device->get(), buf.data(), lsn_t(startLogicalSector), CDIO_READ_MODE_AUDIO, sectors);
        if (drc == DRIVER_OP_SUCCESS) {
            return sectors;
        }
        const char *const errorMessage = cdio_driver_errmsg(drc);
        if (sectors > 1) {
            m_readSectorsLimit = int32_t(sectors / 2);
            qCDebug(dragonMediaBackendNetwork) << "CD read of" << sectors << "sectors at logical sector" << startLogicalSector << "failed:" << errorMessage
                                               << "- retrying with" << m_readSectorsLimit << "sectors";
            sectors = uint32_t(m_readSectorsLimit);
            continue;
        }
        qCDebug(dragonMediaBackendNetwork) << "CD read error at logical sector" << startLogicalSector << ":" << errorMessage;
        return 0;
    }
}

void DragonCdDisc::invalidate()
{
    m_invalid = true;
}
