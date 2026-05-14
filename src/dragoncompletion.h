/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QThread>

namespace DragonSdl
{

struct InitResult {
    bool success = false;
    bool cancelled = false;
    int sampleRate = 0;
    int channels = 0;
    int64_t durationMs = -1;
    bool isGapless = false;
    QString errorMessage;
};

class DragonCompletion
{
public:
    bool await_ready() const
    {
        return m_ready.load(std::memory_order_acquire);
    }

    bool await_suspend(std::coroutine_handle<> h)
    {
        std::lock_guard lock(m_mutex);
        if (m_ready.load(std::memory_order_acquire)) {
            return false;
        }
        m_handle = h;
        return true;
    }

    InitResult await_resume()
    {
        return std::move(*m_result);
    }

    bool setResult(InitResult result)
    {
        std::lock_guard lock(m_mutex);
        if (m_ready.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        m_result = std::move(result);
        if (m_handle) {
            QMetaObject::invokeMethod(
                qApp,
                [h = m_handle]() {
                    if (h) {
                        h.resume();
                    }
                },
                Qt::QueuedConnection);
            m_handle = nullptr;
        }
        return true;
    }

    bool cancel(const QString &reason = {})
    {
        std::lock_guard lock(m_mutex);
        if (m_ready.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        InitResult result;
        result.cancelled = true;
        result.success = false;
        result.errorMessage = reason;
        m_result = std::move(result);
        if (m_handle) {
            QMetaObject::invokeMethod(
                qApp,
                [h = m_handle]() {
                    if (h) {
                        h.resume();
                    }
                },
                Qt::QueuedConnection);
            m_handle = nullptr;
        }
        return true;
    }

private:
    std::optional<InitResult> m_result;
    std::atomic<bool> m_ready{false};
    std::coroutine_handle<> m_handle = nullptr;
    std::mutex m_mutex;
};

}
