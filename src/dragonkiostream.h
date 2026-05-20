/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonstream.h"

#include <KIO/TransferJob>
#include <QByteArray>
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

class DRAGONSDL_EXPORT DragonKioStream : public DragonStream
{
    Q_OBJECT

public:
    explicit DragonKioStream(QObject *parent = nullptr);
    ~DragonKioStream() override;

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, std::stop_token st) override;

    int64_t seek(int64_t offset) override;

    [[nodiscard]] qint64 size() const override;
    [[nodiscard]] qint64 position() const override;

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override;

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

    std::atomic<qint64> m_totalSize{-1};
    std::atomic<qint64> m_streamPosition{0};

    DragonBufferProgress *m_bufferProgress;
};
