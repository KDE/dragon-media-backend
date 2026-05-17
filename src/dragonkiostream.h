/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <KIO/TransferJob>
#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <stop_token>

class DragonBufferProgress;

class DRAGONSDL_EXPORT DragonKioStream : public QObject
{
    Q_OBJECT

public:
    explicit DragonKioStream(QObject *parent = nullptr);
    ~DragonKioStream() override;

    void setUrl(const QUrl &url);
    void start();
    void stop();

    int read(std::span<uint8_t> buf, std::stop_token st);

    [[nodiscard]] DragonBufferProgress *bufferProgress() const;

Q_SIGNALS:
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void onData(KIO::Job *job, const QByteArray &data);
    void onResult(KJob *job);
    void onTotalAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount);
    void onProcessedAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount);

private:
    QUrl m_url;
    QPointer<KIO::TransferJob> m_job;

    std::mutex m_bufferMutex;
    std::condition_variable_any m_bufferCv;
    std::deque<QByteArray> m_networkBuffer;
    int m_bufferOffset{0};

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_error{false};
    std::atomic<bool> m_finished{false};

    DragonBufferProgress *m_bufferProgress;
};
