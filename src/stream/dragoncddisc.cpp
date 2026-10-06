/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncddisc.h"

#include "dragonmediabackend_network_logging.h"

#include <KLocalizedString>
#include <QFileInfo>

#include <cdio++/cdio.hpp>
#include <cdio/disc.h>
#include <cdio/track.h>

#include <algorithm>
#include <cstdlib>
#include <map>

namespace
{

std::mutex s_cacheMutex;
std::map<QString, std::weak_ptr<DragonCdDisc>> s_cache;

QString resolveDevice(const QString &device)
{
    QString resolved = device;
    if (resolved.isEmpty()) {
        if (char *defaultDevice = cdio_get_default_device(nullptr)) {
            resolved = QString::fromLocal8Bit(defaultDevice);
            std::free(defaultDevice);
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

struct Toc {
    DragonCdDisc::Fingerprint fingerprint;
    std::vector<DragonCdDisc::Track> tracks;
};

std::expected<Toc, QString> readToc(CdIo_t *cdio, const QString &device)
{
    const discmode_t discmode = cdio_get_discmode(cdio);
    if (discmode != CDIO_DISC_MODE_CD_DA && discmode != CDIO_DISC_MODE_CD_MIXED) {
        return std::unexpected(i18n("The disc is not an Audio CD."));
    }

    const track_t firstTrack = cdio_get_first_track_num(cdio);
    const track_t numTracks = cdio_get_num_tracks(cdio);
    if (firstTrack == CDIO_INVALID_TRACK || numTracks == CDIO_INVALID_TRACK || numTracks <= 0) {
        return std::unexpected(i18n("The CD has no tracks."));
    }

    Toc toc;
    toc.fingerprint.firstTrack = int(firstTrack);
    toc.fingerprint.lastTrack = int(firstTrack) + int(numTracks) - 1;
    toc.fingerprint.leadOut = cdio_get_track_lsn(cdio, CDIO_CDROM_LEADOUT_TRACK);
    for (int number = toc.fingerprint.firstTrack; number <= toc.fingerprint.lastTrack; ++number) {
        const lsn_t startLsn = cdio_get_track_lsn(cdio, track_t(number));
        const unsigned int secCount = cdio_get_track_sec_count(cdio, track_t(number));
        toc.fingerprint.trackStarts.push_back(startLsn);

        DragonCdDisc::Track track;
        track.number = number;
        track.isAudio = cdio_get_track_format(cdio, track_t(number)) == TRACK_FORMAT_AUDIO;
        if (startLsn != CDIO_INVALID_LSN && secCount > 0) {
            track.firstLsn = startLsn;
            track.lastLsn = int64_t(startLsn) + int64_t(secCount) - 1;
        }
        toc.tracks.push_back(track);
    }

    const QFileInfo info(device);
    if (info.isFile()) {
        toc.fingerprint.fileSize = info.size();
        toc.fingerprint.fileModified = info.lastModified();
    }
    return toc;
}

std::unique_ptr<CdioDevice> openDevice(const QString &device)
{
    auto cdio = std::make_unique<CdioDevice>();
    const QByteArray deviceBytes = device.toLocal8Bit();
    if (!cdio->open(deviceBytes.isEmpty() ? nullptr : deviceBytes.constData(), DRIVER_UNKNOWN)) {
        return nullptr;
    }
    return cdio;
}

}

std::expected<std::shared_ptr<DragonCdDisc>, QString> DragonCdDisc::acquire(const QString &device)
{
    const QString key = resolveDevice(device);

    std::scoped_lock lock(s_cacheMutex);

    if (auto it = s_cache.find(key); it != s_cache.end()) {
        if (auto disc = it->second.lock(); disc && disc->stillCurrent(key)) {
            return disc;
        }
        s_cache.erase(it);
    }

    std::erase_if(s_cache, [](const auto &entry) {
        return entry.second.expired();
    });

    auto opened = open(key, key.isEmpty() ? device : key);
    if (opened) {
        s_cache[key] = *opened;
    }
    return opened;
}

std::expected<std::shared_ptr<DragonCdDisc>, QString> DragonCdDisc::open(const QString &cacheKey, const QString &device)
{
    auto cdio = openDevice(device);
    if (!cdio) {
        return std::unexpected(i18n("Could not open the CD device. Check that a disc is present."));
    }

    auto toc = readToc(cdio->getCdIo(), device);
    if (!toc) {
        return std::unexpected(toc.error());
    }

    // cdio_get_media_changed() reports changes since the last query on the
    // device, so prime it now: the next query then only reports a change
    // that happened while this handle was open.
    cdio_get_media_changed(cdio->getCdIo());

    std::shared_ptr<DragonCdDisc> disc(new DragonCdDisc);
    disc->m_cacheKey = cacheKey;
    disc->m_fingerprint = std::move(toc->fingerprint);
    disc->m_tracks = std::move(toc->tracks);
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
        if (info.size() == m_fingerprint.fileSize && info.lastModified() == m_fingerprint.fileModified) {
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
        changed = cdio_get_media_changed(m_device->getCdIo());
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
    const auto toc = readToc(probe->getCdIo(), device);
    if (!toc || !(toc->fingerprint == m_fingerprint)) {
        qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc TOC changed on" << m_cacheKey;
        return false;
    }
    return true;
}

DragonCdDisc::~DragonCdDisc()
{
    std::scoped_lock lock(m_ioMutex);
    if (m_device) {
        m_device->close();
    }
    qCDebug(dragonMediaBackendNetwork) << "DragonCdDisc closed" << m_cacheKey;
}

const DragonCdDisc::Track *DragonCdDisc::track(int number) const
{
    if (number < firstTrack() || number > lastTrack()) {
        return nullptr;
    }
    return &m_tracks[size_t(number - firstTrack())];
}

uint32_t DragonCdDisc::readAudioSectors(std::span<uint8_t> buf, int64_t lsn, uint32_t sectors)
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
        try {
            m_device->readSectors(buf.data(), lsn_t(lsn), CDIO_READ_MODE_AUDIO, sectors);
        } catch (DriverOpException &e) {
            if (sectors > 1) {
                m_readSectorsLimit = int32_t(sectors / 2);
                qCDebug(dragonMediaBackendNetwork) << "CD read of" << sectors << "sectors at lsn" << lsn << "failed:" << e.get_msg() << "- retrying with"
                                                   << m_readSectorsLimit << "sectors";
                sectors = uint32_t(m_readSectorsLimit);
                continue;
            }
            qCDebug(dragonMediaBackendNetwork) << "CD read error at lsn" << lsn << ":" << e.get_msg();
            return 0;
        }
        return sectors;
    }
}

std::optional<bool> DragonCdDisc::swapLeBe() const
{
    std::scoped_lock lock(m_ioMutex);
    return m_swapLeBe;
}

void DragonCdDisc::setSwapLeBe(bool swap)
{
    std::scoped_lock lock(m_ioMutex);
    m_swapLeBe = swap;
}

void DragonCdDisc::invalidate()
{
    m_invalid = true;
}
