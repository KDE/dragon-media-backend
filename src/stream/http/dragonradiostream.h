/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <dragonicymetadata.h>

#include "dragonicydemuxer.h"
#include "stream/dragonbufferprogress.h"
#include "stream/dragonstream.h"
#include "stream/dragonstreambuffer.h"

class DragonHttpSocket;
class QHttpHeaders;

#include <QByteArray>
#include <QByteArrayView>
#include <QChronoTimer>
#include <QNetworkReply>
#include <QString>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>

class DRAGONMEDIABACKEND_EXPORT DragonRadioStream : public DragonStream
{
    Q_OBJECT

public:
    explicit DragonRadioStream(QObject *parent = nullptr);
    ~DragonRadioStream() override;

    Q_DISABLE_COPY_MOVE(DragonRadioStream)

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, const std::stop_token &st) override;

    qint64 seek(qint64 offset) override;

    [[nodiscard]] bool isSeekable() const override
    {
        return m_acceptsRanges.load();
    }

    [[nodiscard]] qint64 size() const override
    {
        return m_buffer.totalSize();
    }
    [[nodiscard]] qint64 position() const override
    {
        return m_buffer.position();
    }

    [[nodiscard]] bool isAborted() const
    {
        return m_buffer.isAborted();
    }

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override
    {
        return m_buffer.bufferProgress();
    }
    [[nodiscard]] qint64 bufferDepth() const
    {
        return m_buffer.depth();
    }

private:
    void addToAudioBuffer(QByteArrayView data);
    void ingestBody(QByteArrayView data);
    void processMetadata(QByteArrayView metadata);

    void openRequest(std::optional<qint64> rangeStart);
    void restart();
    void prepareForNewTransport();
    void applyResponseHeaders(const QHttpHeaders &headers);
    void handleTransportFinished();
    void handleTransportFailed(QNetworkReply::NetworkError error, const QString &message, bool retryable);
    void reportStreamError(const QString &message);
    [[nodiscard]] bool isBufferFull() const;

    QUrl m_url;

    DragonHttpSocket *m_httpSocket = nullptr;
    QChronoTimer m_reconnectTimer;

    std::atomic<bool> m_acceptsRanges{false};

    DragonIcyDemuxer m_icyDemuxer;

    static constexpr qint64 MAX_BUFFER_BYTES = DragonStreamBuffer::DEFAULT_MAX_BUFFER_BYTES;

    DragonStreamBuffer m_buffer;
    // Radio servers come and go; a connection that fails is worth another try.
    // The budget refreshes whenever a connection answers with a body, so a
    // station that keeps playing for hours never runs out, but one that is
    // simply gone is reported as an error instead of being retried forever.
    static constexpr int MAX_RECONNECTS = 3;
    int m_reconnectAttempts = 0;

    friend class TestRadioStream;
};
