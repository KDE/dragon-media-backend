/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstream.h"

#include "dragonbufferprogress.h"
#include <DragonMediaBackend/dragonicymetadata.h>

#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <stop_token>

class QNetworkAccessManager;

class DRAGONMEDIABACKEND_EXPORT DragonRadioStream : public DragonStream
{
    Q_OBJECT

public:
    explicit DragonRadioStream(QObject *parent = nullptr);
    ~DragonRadioStream() override;

    DragonRadioStream(const DragonRadioStream &) = delete;
    DragonRadioStream &operator=(const DragonRadioStream &) = delete;
    DragonRadioStream(DragonRadioStream &&) = delete;
    DragonRadioStream &operator=(DragonRadioStream &&) = delete;

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, std::stop_token st) override;

    qint64 seek(qint64 offset) override;

    [[nodiscard]] qint64 size() const override;
    [[nodiscard]] qint64 position() const override;

    [[nodiscard]] bool isAborted() const;

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override;

private Q_SLOTS:
    void onReplyEncrypted();
    void onReplyMetaDataChanged();
    void onReplyReadyRead();
    void onReplyFinished();
    void onReplyError(QNetworkReply::NetworkError code);

private:
    void addToAudioBuffer(const QByteArray &data);
    void processIcyData(const QByteArray &data);
    void processMetadata(const QByteArray &metadata);

    QUrl m_url;
    QPointer<QNetworkAccessManager> m_nam;
    QPointer<QNetworkReply> m_reply;

    std::mutex m_bufferMutex;
    std::condition_variable_any m_bufferCv;
    std::deque<QByteArray> m_networkBuffer;
    int m_bufferOffset = 0;

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};
    std::atomic<bool> m_finished{false};

    std::atomic<qint64> m_totalSize{-1};
    std::atomic<qint64> m_streamPosition{0};
    std::atomic<bool> m_acceptsRanges{false};

    DragonBufferProgress *m_bufferProgress;

    int m_icyMetaint = 0;
    int m_icyBytesRead = 0;
    QByteArray m_icyPendingData;
    DragonIcyMetadata m_lastMetadata;

    // Runtime buffer depth tracking
    std::atomic<qint64> m_bufferDepth{0};
    std::atomic<bool> m_isBuffering{false};
    static constexpr qint64 LOW_WATER_MARK = 0;
    static constexpr qint64 HIGH_WATER_MARK = 128 * 1024;
    // Hard cap on buffered bytes. When reached, we stop draining the reply
    // (its own read buffer throttles the socket) instead of growing memory
    // without bound.
    static constexpr qint64 MAX_BUFFER_BYTES = 4 * 1024 * 1024;

    void drainReply();

    friend class TestRadioStream;
};