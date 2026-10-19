/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonstreambuffer.h"

#include "dragonbufferprogress.h"
#include "dragonstream.h"

#include <algorithm>
#include <ranges>

DragonStreamBuffer::DragonStreamBuffer(DragonStream *parentStream, DragonStreamBufferOptions options)
    : m_stream(parentStream)
    , m_options(options)
    , m_bufferProgress(std::make_unique<DragonBufferProgress>())
{
}

DragonStreamBuffer::~DragonStreamBuffer() = default;

void DragonStreamBuffer::setBackpressureRelievedCallback(std::function<void()> callback)
{
    m_backpressureRelievedCallback = std::move(callback);
}

DragonBufferWriteResult DragonStreamBuffer::write(QByteArray data)
{
    if (data.isEmpty()) {
        const qint64 curDepth = m_depth.load();
        return {
            .newDepth = curDepth,
            .exitedBuffering = false,
            .isFull = curDepth >= m_options.maxBufferBytes,
        };
    }

    DragonBufferWriteResult result;
    {
        std::scoped_lock lock(m_mutex);
        const auto size = data.size();
        m_queue.push_back(std::move(data));
        m_depth += size;
        result.newDepth = m_depth.load();

        if (result.newDepth >= m_options.highWaterMark && m_isBuffering.exchange(false)) {
            result.exitedBuffering = true;
        }
        result.isFull = (result.newDepth >= m_options.maxBufferBytes);
    }
    m_cv.notify_all();

    if (result.exitedBuffering && m_stream) {
        Q_EMIT m_stream->streamBuffered();
    }

    return result;
}

DragonBufferWriteResult DragonStreamBuffer::write(QByteArrayView data)
{
    return write(data.toByteArray());
}

DragonBufferReadResult DragonStreamBuffer::readDetailed(std::span<uint8_t> dst, const std::stop_token &st)
{
    if (dst.empty()) {
        return {.bytesRead = 0};
    }

    std::unique_lock lock(m_mutex);

    m_cv.wait(lock, st, [this] {
        return !m_queue.empty() || m_abort.load() || m_error.load() || m_finished.load();
    });

    if (st.stop_requested() || m_abort.load() || m_error.load()) {
        return {.bytesRead = -1};
    }

    if (m_finished.load() && m_queue.empty()) {
        return {.bytesRead = 0};
    }

    size_t bytesRead = 0;
    while (bytesRead < dst.size() && !m_queue.empty()) {
        const QByteArray &front = m_queue.front();
        const auto src = std::as_bytes(std::span(front)).subspan(size_t(m_offset));
        const auto out = std::as_writable_bytes(dst).subspan(bytesRead);
        const size_t take = (std::min)(src.size(), out.size());

        std::ranges::copy(src.first(take), out.begin());
        bytesRead += take;
        m_offset += qsizetype(take);

        if (m_offset >= front.size()) {
            m_queue.pop_front();
            m_offset = 0;
        }
    }

    DragonBufferReadResult result;
    result.bytesRead = static_cast<int>(bytesRead);

    if (bytesRead > 0) {
        m_position += static_cast<qint64>(bytesRead);

        const qint64 oldDepth = m_depth.fetch_sub(static_cast<qint64>(bytesRead));
        const qint64 newDepth = oldDepth - static_cast<qint64>(bytesRead);

        if (oldDepth > m_options.lowWaterMark && newDepth <= m_options.lowWaterMark) {
            if (!m_isBuffering.exchange(true)) {
                result.enteredBuffering = true;
            }
        }
        if (oldDepth >= m_options.maxBufferBytes && newDepth < m_options.maxBufferBytes) {
            result.backpressureRelieved = true;
        }
    }

    lock.unlock();

    if (result.enteredBuffering && m_stream) {
        Q_EMIT m_stream->streamBuffering();
    }

    if (result.backpressureRelieved && m_backpressureRelievedCallback) {
        m_backpressureRelievedCallback();
    }

    return result;
}

int DragonStreamBuffer::read(std::span<uint8_t> dst, const std::stop_token &st)
{
    return readDetailed(dst, st).bytesRead;
}

void DragonStreamBuffer::reset(qint64 position)
{
    {
        std::scoped_lock lock(m_mutex);
        m_abort = false;
        m_error = false;
        m_finished = false;
        m_queue.clear();
        m_offset = 0;
        m_depth = 0;
        m_isBuffering = false;
        m_position = position;
    }
    if (m_bufferProgress) {
        m_bufferProgress->reset();
    }
}

void DragonStreamBuffer::abort()
{
    {
        std::scoped_lock lock(m_mutex);
        m_abort = true;
        m_queue.clear();
        m_offset = 0;
        m_depth = 0;
    }
    m_cv.notify_all();
}

void DragonStreamBuffer::setFinished()
{
    {
        std::scoped_lock lock(m_mutex);
        m_finished = true;
    }
    m_cv.notify_all();
}

void DragonStreamBuffer::setError(const QString &errorMessage)
{
    {
        std::scoped_lock lock(m_mutex);
        m_error = true;
    }
    m_cv.notify_all();

    if (!errorMessage.isEmpty() && m_stream) {
        Q_EMIT m_stream->errorOccurred(errorMessage);
    }
}

bool DragonStreamBuffer::isAborted() const
{
    return m_abort.load();
}

bool DragonStreamBuffer::hasError() const
{
    return m_error.load();
}

bool DragonStreamBuffer::isFinished() const
{
    return m_finished.load();
}

bool DragonStreamBuffer::isEmpty() const
{
    return m_depth.load() == 0;
}

qint64 DragonStreamBuffer::depth() const
{
    return m_depth.load();
}

bool DragonStreamBuffer::isFull() const
{
    return m_depth.load() >= m_options.maxBufferBytes;
}

bool DragonStreamBuffer::isBuffering() const
{
    return m_isBuffering.load();
}

const DragonStreamBufferOptions &DragonStreamBuffer::options() const
{
    return m_options;
}

qint64 DragonStreamBuffer::position() const
{
    return m_position.load();
}

void DragonStreamBuffer::setPosition(qint64 position)
{
    m_position = position;
}

qint64 DragonStreamBuffer::totalSize() const
{
    return m_totalSize.load();
}

void DragonStreamBuffer::setTotalSize(qint64 size)
{
    m_totalSize = size;
}

DragonBufferProgress *DragonStreamBuffer::bufferProgress() const
{
    return m_bufferProgress.get();
}
