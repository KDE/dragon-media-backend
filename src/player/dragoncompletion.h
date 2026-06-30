/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QString>
#include <QThread>

namespace DragonMultimedia
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
    explicit DragonCompletion(QObject *context = nullptr)
        : m_context(context)
    {
    }

    ~DragonCompletion()
    {
        const bool wasResumed = m_resumed.load(std::memory_order_acquire);
        if (m_handle && !wasResumed) {
            qDebug() << "DragonCompletion::~DragonCompletion() destroying un-resumed handle:" << m_handle.address()
                     << "(context was destroyed before posted event fired)";
            m_handle.destroy();
        }
    }

    DragonCompletion(const DragonCompletion &) = delete;
    DragonCompletion &operator=(const DragonCompletion &) = delete;
    DragonCompletion(DragonCompletion &&) = delete;
    DragonCompletion &operator=(DragonCompletion &&) = delete;

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
            qDebug() << "DragonCompletion::setResult() already completed, skipping, handle:" << m_handle.address();
            return false;
        }
        m_result = std::move(result);
        if (m_handle) {
            QObject *receiver = m_context ? m_context : qApp;
            const bool onMainThread = QThread::currentThread() == qApp->thread();
            qDebug() << "DragonCompletion::setResult() scheduling resume via" << (onMainThread ? "QueuedConnection(on-main)" : "QueuedConnection(off-main)")
                     << "handle:" << m_handle.address() << "success:" << m_result->success << "receiver:" << receiver;

            QMetaObject::invokeMethod(
                receiver,
                [h = m_handle, resumed = &m_resumed]() {
                    resumed->store(true, std::memory_order_release);
                    qDebug() << "DragonCompletion::setResult() lambda executing, resuming handle:" << h.address();
                    if (h) {
                        h.resume();
                    }
                    qDebug() << "DragonCompletion::setResult() lambda done, handle:" << h.address();
                },
                Qt::QueuedConnection);

        } else {
            qDebug() << "DragonCompletion::setResult() no handle to resume";
        }
        return true;
    }

    bool cancel(const QString &reason = {})
    {
        std::lock_guard lock(m_mutex);
        if (m_ready.exchange(true, std::memory_order_acq_rel)) {
            qDebug() << "DragonCompletion::cancel() already completed, skipping, handle:" << m_handle.address();
            return false;
        }
        InitResult result;
        result.cancelled = true;
        result.success = false;
        result.errorMessage = reason;
        m_result = std::move(result);
        if (m_handle) {
            QObject *receiver = m_context ? m_context : qApp;
            const bool onMainThread = QThread::currentThread() == qApp->thread();
            qDebug() << "DragonCompletion::cancel() scheduling resume via" << (onMainThread ? "QueuedConnection(on-main)" : "QueuedConnection(off-main)")
                     << "handle:" << m_handle.address() << "reason:" << reason << "receiver:" << receiver;
            QMetaObject::invokeMethod(
                receiver,
                [h = m_handle, resumed = &m_resumed]() {
                    resumed->store(true, std::memory_order_release);
                    qDebug() << "DragonCompletion::cancel() lambda executing, resuming handle:" << h.address();
                    if (h) {
                        h.resume();
                    }
                    qDebug() << "DragonCompletion::cancel() lambda done, handle:" << h.address();
                },
                Qt::QueuedConnection);

        } else {
            qDebug() << "DragonCompletion::cancel() no handle to resume";
        }
        return true;
    }

private:
    QObject *m_context = nullptr;
    std::optional<InitResult> m_result;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_resumed{false};
    std::coroutine_handle<> m_handle = nullptr;
    std::mutex m_mutex;
};

}
