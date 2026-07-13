/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <stdfloat>

#include <LockFreeSpscQueue.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

template<typename T>
class DragonPipe
{
public:
    static constexpr size_t kDefaultCapacity = 65536;

    class Producer
    {
    public:
        Producer() = default;

        size_t write(std::span<const T> items, std::stop_token st);

        template<typename Func>
        size_t writeSomeWith(size_t maxItems, Func &&fn);
        void notify();

        size_t available() const;

    private:
        friend class DragonPipe;
        explicit Producer(DragonPipe *pipe)
            : m_pipe(pipe)
        {
        }
        DragonPipe *m_pipe = nullptr;
    };

    class Consumer
    {
    public:
        Consumer() = default;

        bool waitFor(size_t minItems, std::stop_token st);

        template<typename Func>
        size_t readSomeWith(size_t maxItems, Func &&fn);

        void drain();

        size_t ready() const;

    private:
        friend class DragonPipe;
        explicit Consumer(DragonPipe *pipe)
            : m_pipe(pipe)
        {
        }
        DragonPipe *m_pipe = nullptr;
    };

    explicit DragonPipe(size_t capacity = kDefaultCapacity);

    Producer producer()
    {
        return Producer(this);
    }
    Consumer consumer()
    {
        return Consumer(this);
    }

private:
    std::vector<T> m_buffer;
    LockFreeSpscQueue<T> m_queue;
    std::condition_variable_any m_consumer_cv;
    std::condition_variable_any m_producer_cv;
    std::mutex m_cvMutex;
};

template<typename T>
template<typename Func>
size_t DragonPipe<T>::Producer::writeSomeWith(size_t maxItems, Func &&fn)
{
    size_t n = m_pipe->m_queue.try_write(maxItems, std::forward<Func>(fn));
    if (n > 0) {
        m_pipe->m_consumer_cv.notify_one();
    }
    return n;
}

template<typename T>
template<typename Func>
size_t DragonPipe<T>::Consumer::readSomeWith(size_t maxItems, Func &&fn)
{
    size_t n = m_pipe->m_queue.try_read(maxItems, std::forward<Func>(fn));
    if (n > 0) {
        m_pipe->m_producer_cv.notify_one();
    }
    return n;
}
