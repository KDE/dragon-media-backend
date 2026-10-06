/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstream.h"

#include "dragonmediabackend_export.h"

#include <QUrl>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class CdioDevice;
class DragonBufferProgress;

class DRAGONMEDIABACKEND_EXPORT DragonCdStream : public DragonStream
{
    Q_OBJECT

public:
    static constexpr qsizetype WAV_HEADER_SIZE = 44;
    static constexpr qsizetype SECTOR_SIZE = 2352;
    static constexpr int READ_BLOCK_SECTORS = 25;

    explicit DragonCdStream(QObject *parent = nullptr);
    ~DragonCdStream() override;

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, std::stop_token st) override;

    qint64 seek(qint64 offset) override;

    [[nodiscard]] bool isSeekable() const override
    {
        return true;
    }

    [[nodiscard]] qint64 size() const override;

    [[nodiscard]] qint64 position() const override;

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override;

    [[nodiscard]] static std::optional<int> trackNumberFromName(const QString &fileName, int firstTrackNum, int lastTrackNum);

    /*! Builds the 44-byte RIFF/WAVE header for PCM 2ch 44100 16-bit data. */
    [[nodiscard]] static std::array<uint8_t, 44> makeWavHeader(uint32_t dataSize);

private:
    bool fillCache(int64_t lsn);
    void detectEndianness();
    void fail(const QString &message);

    QUrl m_url;
    DragonBufferProgress *m_bufferProgress = nullptr;

    std::mutex m_ioMutex;
    std::unique_ptr<CdioDevice> m_device;

    std::vector<uint8_t> m_blockCache;
    int64_t m_cacheStartLsn = -1;
    int32_t m_cacheSectors = 0;

    int32_t m_readSectorsLimit = READ_BLOCK_SECTORS;

    std::array<uint8_t, 44> m_wavHeader{};
    int64_t m_firstLsn = 0;
    int64_t m_lastLsn = -1;
    bool m_swapLeBe = false;

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};

    std::atomic<qint64> m_totalSize{-1};
    std::atomic<qint64> m_streamPosition{0};

    friend class TestCdStream;
};
