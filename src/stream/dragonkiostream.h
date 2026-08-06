/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
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
class QTimer;

class DRAGONMULTIMEDIA_EXPORT DragonKioStream : public DragonStream
{
    Q_OBJECT

public:
    explicit DragonKioStream(QObject *parent = nullptr);
    ~DragonKioStream() override;

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, std::stop_token st) override;

    qint64 seek(qint64 offset) override;

    [[nodiscard]] qint64 size() const override;
    [[nodiscard]] qint64 position() const override;

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override;

private Q_SLOTS:
    void onData(KIO::Job *job, const QByteArray &data);
    void onResult(KJob *job);
    void onTotalAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount);
    void onProcessedAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount);
    void onWatchdogTimeout();

private:
    QUrl m_url;
    QPointer<KIO::TransferJob> m_job;
    QPointer<QTimer> m_watchdogTimer;

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

    // Runtime buffer depth tracking
    std::atomic<qint64> m_bufferDepth{0};
    std::atomic<bool> m_isBuffering{false};
    static constexpr qint64 LOW_WATER_MARK = 0;
    static constexpr qint64 HIGH_WATER_MARK = 128 * 1024;
    // Hard cap on buffered bytes. When reached, the KIO job is suspended to
    // apply backpressure instead of growing memory without bound.
    static constexpr qint64 MAX_BUFFER_BYTES = 4 * 1024 * 1024;
    bool m_suspended = false;

    void applyBackpressure();
    void releaseBackpressure();

Q_SIGNALS:
    void backpressureReleased();

    friend class TestKioStream;
};
