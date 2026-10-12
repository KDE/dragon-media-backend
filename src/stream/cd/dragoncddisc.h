/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <dragoncdtoc.h>

#include <QDateTime>
#include <QString>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

class CdIoHandle;

class DRAGONMEDIABACKEND_EXPORT DragonCdDisc
{
public:
    static constexpr int SECTOR_SIZE = 2352;
    static constexpr int READ_BLOCK_SECTORS = 25;

    [[nodiscard]] static std::expected<std::shared_ptr<DragonCdDisc>, QString> acquire(const QString &device);

    ~DragonCdDisc();
    DragonCdDisc(const DragonCdDisc &) = delete;
    DragonCdDisc &operator=(const DragonCdDisc &) = delete;

    [[nodiscard]] const DragonCdToc &toc() const
    {
        return m_toc;
    }

    [[nodiscard]] int firstTrack() const;
    [[nodiscard]] int lastTrack() const;
    [[nodiscard]] bool isAudioTrack(int number) const;
    [[nodiscard]] std::pair<int64_t, int64_t> trackLogicalSectorRange(int number) const;
    [[nodiscard]] const QString &cacheKey() const
    {
        return m_cacheKey;
    }

    [[nodiscard]] uint32_t readAudioSectors(std::span<uint8_t> buf, int64_t startLogicalSector, uint32_t sectors);

    void invalidate();
    [[nodiscard]] bool isValid() const
    {
        return !m_invalid.load();
    }

private:
    DragonCdDisc() = default;

    static std::expected<DragonCdToc, QString> readToc(CdIoHandle &cdio, const QString &device);
    static std::expected<std::shared_ptr<DragonCdDisc>, QString> open(const QString &cacheKey, const QString &device);
    bool stillCurrent(const QString &device);

    QString m_cacheKey;
    DragonCdToc m_toc;
    qint64 m_fileSize = -1;
    QDateTime m_fileModified;

    mutable std::mutex m_ioMutex;
    std::unique_ptr<CdIoHandle> m_device;
    int32_t m_readSectorsLimit = READ_BLOCK_SECTORS;

    std::atomic<bool> m_invalid{false};

    friend class TestCdStream;
};
