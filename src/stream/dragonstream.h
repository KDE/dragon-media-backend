/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>
#include <QUrl>

#include <cstdint>
#include <span>
#include <stop_token>

class DragonBufferProgress;
class DragonIcyMetadata;

class DRAGONMEDIABACKEND_EXPORT DragonStream : public QObject
{
    Q_OBJECT
public:
    explicit DragonStream(QObject *parent = nullptr);
    ~DragonStream() override = default;

    virtual void setUrl(const QUrl &url) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

    virtual int read(std::span<uint8_t> buf, std::stop_token st) = 0;

    virtual qint64 seek(qint64 offset)
    {
        Q_UNUSED(offset);
        return -1;
    }

    virtual qint64 size() const
    {
        return -1;
    }

    virtual qint64 position() const
    {
        return -1;
    }

    virtual DragonBufferProgress *bufferProgress() const = 0;

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void metadataReady(const DragonIcyMetadata &metadata);
    void streamStalled();
    void streamBuffering();
    void streamBuffered();
};
