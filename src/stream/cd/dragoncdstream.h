/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "stream/dragonstream.h"

#include "dragonmediabackend_export.h"

#include <QUrl>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class DragonBufferProgress;
class DragonCdDisc;

class DRAGONMEDIABACKEND_EXPORT DragonCdStream : public DragonStream
{
    Q_OBJECT

public:
    static constexpr qsizetype WAV_HEADER_SIZE = 44;

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

    /*! Builds the 44-byte RIFF/WAVE header for PCM 2ch 44100 16-bit data. */
    [[nodiscard]] static std::array<uint8_t, 44> makeWavHeader(uint32_t dataSize);

private:
    /*! Fills the read-ahead cache from the disc starting at \a
     * startLogicalSector. Returns the failure message on error. Callers run
     * under m_ioMutex and relay the message out of the locked section
     * instead of emitting errorOccurred there, so a directly connected slot
     * may reenter stop()/seek()/read().
     */
    std::expected<void, QString> fillCache(qint64 startLogicalSector);

    /*! Sets the error state (and drops the disc handle) under the I/O mutex.
     * Does not emit errorOccurred: callers emit once the lock is released,
     * so a directly connected slot may reenter stop()/seek()/read().
     */
    void fail(const QString &message);

    /*! read() body, called with m_ioMutex held. Returns the failure message
     * instead of emitting; read() emits after unlocking. A returned value of
     * -1 signals a stop or an error state without a message, 0 the end of the
     * stream.
     */
    std::expected<int, QString> readLocked(std::span<uint8_t> buf, std::stop_token st);

    QUrl m_url;
    DragonBufferProgress *m_bufferProgress = nullptr;

    std::mutex m_ioMutex;
    std::shared_ptr<DragonCdDisc> m_disc;

    std::vector<uint8_t> m_blockCache;
    qint64 m_cacheStartLogicalSector = -1;
    int32_t m_cacheSectors = 0;
    std::array<uint8_t, 44> m_wavHeader{};
    qint64 m_firstLogicalSector = 0;
    qint64 m_lastLogicalSector = -1;

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};

    std::atomic<qint64> m_totalSize{-1};
    std::atomic<qint64> m_streamPosition{0};

    friend class TestCdStream;
};
