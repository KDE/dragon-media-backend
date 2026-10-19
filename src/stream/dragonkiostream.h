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

#include "dragonstreambuffer.h"

class DragonBufferProgress;
class QTimer;

class DRAGONMEDIABACKEND_EXPORT DragonKioStream : public DragonStream
{
    Q_OBJECT

public:
    explicit DragonKioStream(QObject *parent = nullptr);
    ~DragonKioStream() override;

    void setUrl(const QUrl &url) override;
    void start() override;
    void stop() override;

    int read(std::span<uint8_t> buf, const std::stop_token &st) override;

    qint64 seek(qint64 offset) override;

    [[nodiscard]] bool isSeekable() const override
    {
        return m_url.scheme() == QLatin1String("sftp");
    }

    [[nodiscard]] qint64 size() const override
    {
        return m_buffer.totalSize();
    }
    [[nodiscard]] qint64 position() const override
    {
        return m_buffer.position();
    }

    [[nodiscard]] DragonBufferProgress *bufferProgress() const override
    {
        return m_buffer.bufferProgress();
    }
    [[nodiscard]] qint64 bufferDepth() const
    {
        return m_buffer.depth();
    }
    [[nodiscard]] bool isAborted() const
    {
        return m_buffer.isAborted();
    }

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

    static constexpr qint64 MAX_BUFFER_BYTES = DragonStreamBuffer::DEFAULT_MAX_BUFFER_BYTES;

    DragonStreamBuffer m_buffer;
    bool m_suspended = false;

    void applyBackpressure();
    void releaseBackpressure();

Q_SIGNALS:
    void backpressureReleased();

    friend class TestKioStream;
};
