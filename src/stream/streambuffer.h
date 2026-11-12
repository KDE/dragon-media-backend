/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>

class Stream;
class BufferProgress;

struct DRAGONMEDIABACKEND_EXPORT StreamBufferOptions {
    qint64 lowWaterMark = 0;
    qint64 highWaterMark = 128 * 1024;
    qint64 maxBufferBytes = 4 * 1024 * 1024;
};

struct DRAGONMEDIABACKEND_EXPORT BufferReadResult {
    int bytesRead = 0;
    bool enteredBuffering = false;
    bool backpressureRelieved = false;
};

struct DRAGONMEDIABACKEND_EXPORT BufferWriteResult {
    qint64 newDepth = 0;
    bool exitedBuffering = false;
    bool isFull = false;
};

/**
 * Thread-safe FIFO byte queue bridging an asynchronous network/IO producer
 * with a synchronous blocking reader (e.g. FFmpeg decode thread).
 *
 * Manages buffer depth, stream position, total size, progress reporting,
 * high/low watermarks with Qt signal emission on Stream, and
 * backpressure thresholding.
 */
class DRAGONMEDIABACKEND_EXPORT StreamBuffer
{
public:
    static constexpr qint64 DEFAULT_LOW_WATER_MARK = 0;
    static constexpr qint64 DEFAULT_HIGH_WATER_MARK = 128 * 1024;
    static constexpr qint64 RADIO_HIGH_WATER_MARK = 16 * 1024;
    static constexpr qint64 DEFAULT_MAX_BUFFER_BYTES = 4 * 1024 * 1024;

    explicit StreamBuffer(Stream *parentStream = nullptr, const StreamBufferOptions &options = {});
    ~StreamBuffer();

    StreamBuffer(const StreamBuffer &) = delete;
    StreamBuffer &operator=(const StreamBuffer &) = delete;
    StreamBuffer(StreamBuffer &&) = delete;
    StreamBuffer &operator=(StreamBuffer &&) = delete;

    void setBackpressureRelievedCallback(std::function<void()> callback);

    BufferWriteResult write(QByteArray data);
    BufferWriteResult write(QByteArrayView data);

    int read(std::span<uint8_t> dst, const std::stop_token &st);
    BufferReadResult readDetailed(std::span<uint8_t> dst, const std::stop_token &st);

    void reset(qint64 position = 0);
    void abort();
    void setFinished();
    void setError(const QString &errorMessage = {});

    [[nodiscard]] bool isAborted() const;
    [[nodiscard]] bool hasError() const;
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] qint64 depth() const;
    [[nodiscard]] bool isFull() const;
    [[nodiscard]] bool isBuffering() const;
    [[nodiscard]] const StreamBufferOptions &options() const;

    [[nodiscard]] qint64 position() const;
    void setPosition(qint64 position);

    [[nodiscard]] qint64 totalSize() const;
    void setTotalSize(qint64 size);

    [[nodiscard]] BufferProgress *bufferProgress() const;

private:
    Stream *m_stream = nullptr;
    const StreamBufferOptions m_options;

    mutable std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<QByteArray> m_queue;
    qsizetype m_offset = 0;

    std::atomic<qint64> m_depth{0};
    std::atomic<qint64> m_position{0};
    std::atomic<qint64> m_totalSize{-1};
    std::atomic<bool> m_isBuffering{false};
    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};
    std::atomic<bool> m_finished{false};

    std::function<void()> m_backpressureRelievedCallback;

    std::unique_ptr<BufferProgress> m_bufferProgress;
};
