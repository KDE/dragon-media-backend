/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncompletion.h"

#include <QCoreApplication>
#include <QDebug>

namespace DragonMultimedia
{

DragonCompletion::DragonCompletion()
    : QObject(nullptr) // unparented managed by shared_ptr
{
}

DragonCompletion::~DragonCompletion()
{
    // If a coroutine is still suspended waiting on us and we're being
    // destroyed (e.g. the owning pipeline was torn down), resume it with
    // a cancelled result so its frame is cleaned up by QCoro rather than
    // leaked. This goes through the normal finished() signal path (guarded
    // by the awaiter's atomic `resumed` flag), so the coroutine runs its
    // normal cleanup instead of being forcibly destroyed out from under
    // QCoro's ref-counting.
    if (!m_ready.exchange(true, std::memory_order_acq_rel)) {
        qDebug() << "DragonCompletion::~DragonCompletion() resuming suspended awaiter with cancelled result, this:" << this;
        InitResult result;
        result.cancelled = true;
        result.success = false;
        result.errorMessage = QStringLiteral("Completion destroyed");
        m_buffered = std::move(result);
        Q_EMIT finished(*m_buffered);
    }
}

bool DragonCompletion::isReady() const
{
    return m_ready.load(std::memory_order_acquire);
}

InitResult DragonCompletion::result() const
{
    return *m_buffered;
}

bool DragonCompletion::setResult(InitResult result)
{
    if (m_ready.exchange(true, std::memory_order_acq_rel)) {
        qDebug() << "DragonCompletion::setResult() already completed, skipping, this:" << this;
        return false;
    }
    m_buffered = std::move(result);
    qDebug() << "DragonCompletion::setResult() emitting finished, this:" << this << "success:" << m_buffered->success;
    Q_EMIT finished(*m_buffered);
    return true;
}

bool DragonCompletion::cancel(const QString &reason)
{
    if (m_ready.exchange(true, std::memory_order_acq_rel)) {
        qDebug() << "DragonCompletion::cancel() already completed, skipping, this:" << this;
        return false;
    }
    InitResult result;
    result.cancelled = true;
    result.success = false;
    result.errorMessage = reason;
    m_buffered = std::move(result);
    qDebug() << "DragonCompletion::cancel() emitting finished, this:" << this << "reason:" << reason;
    Q_EMIT finished(*m_buffered);
    return true;
}

}
