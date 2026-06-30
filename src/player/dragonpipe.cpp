/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpipe.h"

#include "dragonmultimedia_export.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <ranges>
#include <thread>

template<typename T>
DragonPipe<T>::DragonPipe(size_t capacity)
    : m_buffer(capacity)
    , m_queue(std::span<T>(m_buffer))
{
    assert((capacity & (capacity - 1)) == 0 && "Capacity must be a power of two");
}

template<typename T>
size_t DragonPipe<T>::Producer::write(std::span<const T> items, std::stop_token st)
{
    size_t written = 0;
    while (written < items.size() && !st.stop_requested()) {
        auto remaining = items.subspan(written);
        size_t n = m_pipe->m_queue.try_write(remaining.size(), [&](std::span<T> b1, std::span<T> b2) {
            auto in_iter = std::ranges::copy_n(remaining.begin(), b1.size(), b1.begin()).in;
            std::ranges::copy_n(in_iter, b2.size(), b2.begin());
        });
        if (n > 0) {
            m_pipe->m_consumer_cv.notify_one();
        }
        written += n;
        if (written < items.size() && !st.stop_requested()) {
            std::unique_lock lock(m_pipe->m_cvMutex);
            m_pipe->m_producer_cv.wait_for(lock, st, std::chrono::milliseconds(1), [&] {
                return m_pipe->m_queue.get_num_free() > 0;
            });
        }
    }
    return written;
}

template<typename T>
void DragonPipe<T>::Producer::notify()
{
    m_pipe->m_consumer_cv.notify_one();
}

template<typename T>
size_t DragonPipe<T>::Producer::available() const
{
    return m_pipe->m_queue.get_num_free();
}

template<typename T>
bool DragonPipe<T>::Consumer::waitFor(size_t minItems, std::stop_token st)
{
    std::unique_lock lock(m_pipe->m_cvMutex);
    bool ready = m_pipe->m_consumer_cv.wait_for(lock, st, std::chrono::milliseconds(50), [&] {
        return m_pipe->m_queue.get_num_items_ready() >= minItems;
    });
    return ready && !st.stop_requested();
}

template<typename T>
void DragonPipe<T>::Consumer::drain()
{
    while (true) {
        size_t n = m_pipe->m_queue.get_num_items_ready();
        if (n == 0)
            break;
        size_t discarded = m_pipe->m_queue.try_read(n, [](std::span<const T>, std::span<const T>) { });
        if (discarded > 0) {
            m_pipe->m_producer_cv.notify_one();
        }
    }
}

template<typename T>
size_t DragonPipe<T>::Consumer::ready() const
{
    return m_pipe->m_queue.get_num_items_ready();
}

template class DRAGONMULTIMEDIA_EXPORT DragonPipe<std::float32_t>;
#include "fft/dragonfftblock.h"
template class DRAGONMULTIMEDIA_EXPORT DragonPipe<DragonFftBlock>;
