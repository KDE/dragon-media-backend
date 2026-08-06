/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragonbufferprogress.h>

#include <algorithm>

DragonBufferProgress::DragonBufferProgress(QObject *parent)
    : QObject(parent)
{
}

void DragonBufferProgress::reset()
{
    m_tlsHandshakeComplete = false;
    m_headersReceived = false;
    m_bytesReceived = 0;
    m_decoderReady = false;
    m_lastEmittedProgress = -1.0;
    Q_EMIT progressChanged(0.0);
}

void DragonBufferProgress::setTlsHandshakeComplete(bool complete)
{
    if (m_tlsHandshakeComplete != complete) {
        m_tlsHandshakeComplete = complete;
        emitProgressIfChanged();
    }
}

void DragonBufferProgress::setHeadersReceived(bool received)
{
    if (m_headersReceived != received) {
        m_headersReceived = received;
        emitProgressIfChanged();
    }
}

void DragonBufferProgress::setBytesReceived(qint64 bytes)
{
    if (m_bytesReceived != bytes) {
        m_bytesReceived = bytes;
        emitProgressIfChanged();
    }
}

void DragonBufferProgress::setDecoderReady(bool ready)
{
    if (m_decoderReady != ready) {
        m_decoderReady = ready;
        emitProgressIfChanged();
    }
}

qreal DragonBufferProgress::progress() const
{
    if (m_decoderReady)
        return 1.0;
    if (!m_tlsHandshakeComplete)
        return 0.0;
    if (!m_headersReceived)
        return 0.30;

    constexpr qint64 READY_THRESHOLD = 65536;
    qreal downloadRatio = std::min(1.0, static_cast<qreal>(m_bytesReceived) / READY_THRESHOLD);
    return 0.50 + (downloadRatio * 0.45);
}

bool DragonBufferProgress::headersReceived() const
{
    return m_headersReceived;
}

bool DragonBufferProgress::isTlsHandshakeComplete() const
{
    return m_tlsHandshakeComplete;
}

void DragonBufferProgress::emitProgressIfChanged()
{
    qreal current = progress();
    if (current > m_lastEmittedProgress) {
        m_lastEmittedProgress = current;
        Q_EMIT progressChanged(current);
    }
}
