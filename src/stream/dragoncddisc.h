/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QDateTime>
#include <QString>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

class CdioDevice;

class DRAGONMEDIABACKEND_EXPORT DragonCdDisc
{
public:
    static constexpr int SECTOR_SIZE = 2352;
    static constexpr int READ_BLOCK_SECTORS = 25;

    struct Track {
        int number = 0;
        bool isAudio = false;
        int64_t firstLsn = 0;
        int64_t lastLsn = -1;
    };

    /*!
     * Identity of the inserted disc: the same inputs as a MusicBrainz/freedb
     * disc ID (track range, every track start and the lead-out), plus the
     * size and modification time when the device is an image file.
     */
    struct Fingerprint {
        int firstTrack = 0;
        int lastTrack = -1;
        std::vector<int64_t> trackStarts;
        int64_t leadOut = -1;
        qint64 fileSize = -1;
        QDateTime fileModified;

        bool operator==(const Fingerprint &) const = default;
    };

    [[nodiscard]] static std::expected<std::shared_ptr<DragonCdDisc>, QString> acquire(const QString &device);

    ~DragonCdDisc();
    DragonCdDisc(const DragonCdDisc &) = delete;
    DragonCdDisc &operator=(const DragonCdDisc &) = delete;

    [[nodiscard]] int firstTrack() const
    {
        return m_fingerprint.firstTrack;
    }
    [[nodiscard]] int lastTrack() const
    {
        return m_fingerprint.lastTrack;
    }
    [[nodiscard]] const Track *track(int number) const;

    [[nodiscard]] const Fingerprint &fingerprint() const
    {
        return m_fingerprint;
    }
    [[nodiscard]] const QString &cacheKey() const
    {
        return m_cacheKey;
    }

    [[nodiscard]] uint32_t readAudioSectors(std::span<uint8_t> buf, int64_t lsn, uint32_t sectors);

    [[nodiscard]] std::optional<bool> swapLeBe() const;
    void setSwapLeBe(bool swap);

    /*! Marks the disc unusable (e.g. after a fatal read error). */
    void invalidate();
    [[nodiscard]] bool isValid() const
    {
        return !m_invalid.load();
    }

private:
    DragonCdDisc() = default;

    static std::expected<std::shared_ptr<DragonCdDisc>, QString> open(const QString &cacheKey, const QString &device);
    bool stillCurrent(const QString &device);

    QString m_cacheKey;
    Fingerprint m_fingerprint;
    std::vector<Track> m_tracks;

    mutable std::mutex m_ioMutex;
    std::unique_ptr<CdioDevice> m_device;
    int32_t m_readSectorsLimit = READ_BLOCK_SECTORS;
    std::optional<bool> m_swapLeBe;

    std::atomic<bool> m_invalid{false};

    friend class TestCdStream;
};
