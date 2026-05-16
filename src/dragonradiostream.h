/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <dragonbufferprogress.h>
#include <dragonsdl/dragonicymetadata.h>

#include <QNetworkReply>
#include <QObject>
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
class QTimer;

class DRAGONSDL_EXPORT DragonRadioStream : public QObject
{
    Q_OBJECT

public:
    explicit DragonRadioStream(QObject *parent = nullptr);
    ~DragonRadioStream() override;

    DragonRadioStream(const DragonRadioStream &) = delete;
    DragonRadioStream &operator=(const DragonRadioStream &) = delete;
    DragonRadioStream(DragonRadioStream &&) = delete;
    DragonRadioStream &operator=(DragonRadioStream &&) = delete;

    void setUrl(const QUrl &url);
    void start();
    void stop();

    int read(std::span<uint8_t> buf, std::stop_token st);

    [[nodiscard]] bool isAborted() const;

    [[nodiscard]] DragonBufferProgress *bufferProgress() const;

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void metadataReady(const DragonIcyMetadata &metadata);

private Q_SLOTS:
    void onReplyEncrypted();
    void onReplyMetaDataChanged();
    void onReplyReadyRead();
    void onReplyFinished();
    void onReplyError(QNetworkReply::NetworkError code);
    void onWatchdogTimeout();

private:
    void addToAudioBuffer(const QByteArray &data);
    void processIcyData(const QByteArray &data);
    void processMetadata(const QByteArray &metadata);

    QUrl m_url;
    QPointer<QNetworkAccessManager> m_nam;
    QPointer<QNetworkReply> m_reply;
    QPointer<QTimer> m_watchdogTimer;

    std::mutex m_bufferMutex;
    std::condition_variable_any m_bufferCv;
    std::deque<QByteArray> m_networkBuffer;
    int m_bufferOffset = 0;

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};
    std::atomic<bool> m_finished{false};

    DragonBufferProgress *m_bufferProgress;

    int m_icyMetaint = 0;
    int m_icyBytesRead = 0;
    QByteArray m_icyPendingData;
    DragonIcyMetadata m_lastMetadata;
};