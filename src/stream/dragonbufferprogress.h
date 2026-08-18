/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>

class DRAGONMEDIABACKEND_EXPORT DragonBufferProgress : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)

public:
    explicit DragonBufferProgress(QObject *parent = nullptr);

    void reset();

    void setTlsHandshakeComplete(bool complete);
    void setHeadersReceived(bool received);
    void setBytesReceived(qint64 bytes);
    void setDecoderReady(bool ready);

    [[nodiscard]] qreal progress() const;
    [[nodiscard]] bool headersReceived() const;
    [[nodiscard]] bool isTlsHandshakeComplete() const;

Q_SIGNALS:
    void progressChanged(qreal progress);

private:
    bool m_tlsHandshakeComplete = false;
    bool m_headersReceived = false;
    qint64 m_bytesReceived = 0;
    bool m_decoderReady = false;
    qreal m_lastEmittedProgress = -1.0;

    void emitProgressIfChanged();
};
