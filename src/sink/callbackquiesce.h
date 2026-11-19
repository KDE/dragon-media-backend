/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

/**
 * Coordinates audio-sink teardown with real-time audio callbacks so native
 * audio objects are not freed while a callback is still using them, without
 * taking a mutex on the real-time thread.
 */
class CallbackQuiesce
{
public:
    static constexpr std::chrono::milliseconds kDefaultCloseTimeout{1500};

    CallbackQuiesce() = default;

    CallbackQuiesce(const CallbackQuiesce &) = delete;
    CallbackQuiesce &operator=(const CallbackQuiesce &) = delete;

    class ScopedCallback
    {
    public:
        explicit ScopedCallback(CallbackQuiesce &quiesce)
            : m_quiesce(&quiesce)
            , m_closing(quiesce.enterCallback())
        {
        }

        ~ScopedCallback()
        {
            m_quiesce->leaveCallback();
        }

        ScopedCallback(const ScopedCallback &) = delete;
        ScopedCallback &operator=(const ScopedCallback &) = delete;

        [[nodiscard]] bool closing() const
        {
            return m_closing;
        }

    private:
        CallbackQuiesce *m_quiesce;
        bool m_closing;
    };

    void beginClose()
    {
        m_closing.store(true, std::memory_order_seq_cst);
    }

    [[nodiscard]] bool isClosing() const
    {
        return m_closing.load(std::memory_order_seq_cst);
    }

    [[nodiscard]] bool waitForIdle(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (m_inFlight.load(std::memory_order_seq_cst) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return m_inFlight.load(std::memory_order_seq_cst) == 0;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    [[nodiscard]] bool isIdle() const
    {
        return m_inFlight.load(std::memory_order_seq_cst) == 0;
    }

private:
    friend class ScopedCallback;

    // Increment-then-check with seq_cst guarantees either the callback sees
    // beginClose() and bails out, or waitForIdle() sees this callback in flight.
    [[nodiscard]] bool enterCallback()
    {
        m_inFlight.fetch_add(1, std::memory_order_seq_cst);
        return m_closing.load(std::memory_order_seq_cst);
    }

    void leaveCallback()
    {
        m_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        m_inFlight.notify_all();
    }

    std::atomic<int> m_inFlight{0};
    std::atomic<bool> m_closing{false};
};
