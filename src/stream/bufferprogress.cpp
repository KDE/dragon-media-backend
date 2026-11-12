/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <bufferprogress.h>

#include <algorithm>

BufferProgress::BufferProgress(QObject *parent)
    : QObject(parent)
{
}

void BufferProgress::reset()
{
    m_tlsHandshakeComplete = false;
    m_headersReceived = false;
    m_bytesReceived = 0;
    m_decoderReady = false;
    m_lastEmittedProgress = -1.0;
    Q_EMIT progressChanged(0.0);
}

void BufferProgress::setTlsHandshakeComplete(bool complete)
{
    if (m_tlsHandshakeComplete != complete) {
        m_tlsHandshakeComplete = complete;
        emitProgressIfChanged();
    }
}

void BufferProgress::setHeadersReceived(bool received)
{
    if (m_headersReceived != received) {
        m_headersReceived = received;
        emitProgressIfChanged();
    }
}

void BufferProgress::setBytesReceived(qint64 bytes)
{
    if (m_bytesReceived != bytes) {
        m_bytesReceived = bytes;
        emitProgressIfChanged();
    }
}

void BufferProgress::setDecoderReady(bool ready)
{
    if (m_decoderReady != ready) {
        m_decoderReady = ready;
        emitProgressIfChanged();
    }
}

qreal BufferProgress::progress() const
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

void BufferProgress::emitProgressIfChanged()
{
    qreal current = progress();
    if (current > m_lastEmittedProgress) {
        m_lastEmittedProgress = current;
        Q_EMIT progressChanged(current);
    }
}
