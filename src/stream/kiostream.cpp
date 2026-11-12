/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "kiostream.h"

#include "bufferprogress.h"
#include "dragonmediabackend_network_logging.h"

#include <KIO/TransferJob>
#include <QTimer>
#include <QUrl>

KioStream::KioStream(QObject *parent)
    : Stream(parent)
    , m_watchdogTimer(new QTimer(this))
    , m_buffer(this)
{
    m_watchdogTimer->setInterval(5000);
    m_watchdogTimer->setSingleShot(true);
    connect(m_watchdogTimer, &QTimer::timeout, this, &KioStream::onWatchdogTimeout);

    m_buffer.setBackpressureRelievedCallback([this]() {
        releaseBackpressure();
    });
}

KioStream::~KioStream()
{
    KioStream::stop();
}

void KioStream::setUrl(const QUrl &url)
{
    m_url = url;
}

void KioStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "KioStream start" << m_url.toString();

    Q_EMIT seekableChanged(isSeekable());

    m_buffer.reset();
    m_suspended = false;

    if (m_job) {
        m_job->kill(KJob::Quietly);
    }

    m_job = KIO::get(m_url, KIO::NoReload, KIO::HideProgressInfo);

    connect(m_job, &KIO::TransferJob::data, this, &KioStream::onData);
    connect(m_job, &KJob::result, this, &KioStream::onResult);
    connect(m_job, &KJob::totalAmountChanged, this, &KioStream::onTotalAmountChanged);
    connect(m_job, &KJob::processedAmountChanged, this, &KioStream::onProcessedAmountChanged);
    connect(this, &KioStream::backpressureReleased, m_job, &KJob::resume, Qt::QueuedConnection);

    m_watchdogTimer->start();
}

void KioStream::stop()
{
    m_buffer.abort();

    if (m_watchdogTimer) {
        m_watchdogTimer->stop();
    }

    if (m_job) {
        disconnect(m_job, nullptr, this, nullptr);
        m_job->kill(KJob::Quietly);
        m_job = nullptr;
    }
    m_suspended = false;
}

int KioStream::read(std::span<uint8_t> buf, const std::stop_token &st)
{
    return m_buffer.read(buf, st);
}

void KioStream::applyBackpressure()
{
    // Runs on the KIO job's thread (from onData). Suspending throttles
    // further network delivery so the buffer cannot grow without bound when
    // the consumer (decode thread) is slower than the network.
    if (!m_suspended && m_job && m_buffer.isFull()) {
        qCDebug(dragonMediaBackendNetwork) << "KIO buffer full (" << m_buffer.depth() << "bytes), suspending job";
        m_suspended = true;
        m_job->suspend();
    }
}

void KioStream::releaseBackpressure()
{
    // May run on the decode thread; emit a queued signal so the job
    // resumes on its own thread.
    if (m_suspended && m_job) {
        m_suspended = false;
        Q_EMIT backpressureReleased();
    }
}

qint64 KioStream::seek(qint64 offset)
{
    stop();

    m_buffer.reset(offset);
    m_suspended = false;

    m_job = KIO::get(m_url, KIO::NoReload, KIO::HideProgressInfo);

    m_job->addMetaData(QStringLiteral("resume"), QString::number(offset));

    connect(m_job, &KIO::TransferJob::data, this, &KioStream::onData);
    connect(m_job, &KJob::result, this, &KioStream::onResult);
    connect(m_job, &KJob::totalAmountChanged, this, &KioStream::onTotalAmountChanged);
    connect(m_job, &KJob::processedAmountChanged, this, &KioStream::onProcessedAmountChanged);
    connect(this, &KioStream::backpressureReleased, m_job, &KJob::resume, Qt::QueuedConnection);

    m_watchdogTimer->start();

    return offset;
}

void KioStream::onData(KIO::Job *job, const QByteArray &data)
{
    if (job != m_job || data.isEmpty()) {
        return;
    }

    if (m_watchdogTimer) {
        m_watchdogTimer->start();
    }

    m_buffer.write(data);
    applyBackpressure();
}

void KioStream::onResult(KJob *job)
{
    if (job != m_job) {
        return;
    }

    if (m_watchdogTimer) {
        m_watchdogTimer->stop();
    }

    if (job->error()) {
        qCWarning(dragonMediaBackendNetwork) << "KIO error:" << job->errorString();
        m_buffer.setError(job->errorString());
    } else {
        qCDebug(dragonMediaBackendNetwork) << "KIO job finished successfully";
        m_buffer.setFinished();
    }
}

void KioStream::onTotalAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount)
{
    if (job != m_job) {
        return;
    }
    if (unit == KJob::Bytes) {
        m_buffer.setTotalSize(static_cast<qint64>(amount));
    }
}

void KioStream::onProcessedAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount)
{
    if (job != m_job) {
        return;
    }
    if (unit == KJob::Bytes) {
        m_buffer.bufferProgress()->setBytesReceived(static_cast<qint64>(amount));
    }
}

void KioStream::onWatchdogTimeout()
{
    if (m_buffer.isAborted()) {
        return;
    }

    qCDebug(dragonMediaBackendNetwork) << "KIO watchdog timeout reconnecting";
    Q_EMIT streamStalled();

    if (m_job) {
        m_job->kill(KJob::Quietly);
        m_job = nullptr;
    }
    start();
}
