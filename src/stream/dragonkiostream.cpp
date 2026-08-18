/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonkiostream.h"

#include "dragonbufferprogress.h"
#include "dragonmediabackend_network_logging.h"

#include <KIO/TransferJob>
#include <QTimer>
#include <QUrl>

DragonKioStream::DragonKioStream(QObject *parent)
    : DragonStream(parent)
    , m_watchdogTimer(new QTimer(this))
    , m_bufferProgress(new DragonBufferProgress(this))
{
    m_watchdogTimer->setInterval(5000);
    m_watchdogTimer->setSingleShot(true);
    connect(m_watchdogTimer, &QTimer::timeout, this, &DragonKioStream::onWatchdogTimeout);
}

DragonKioStream::~DragonKioStream()
{
    DragonKioStream::stop();
}

void DragonKioStream::setUrl(const QUrl &url)
{
    m_url = url;
}

void DragonKioStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "DragonKioStream start" << m_url.toString();

    m_abort = false;
    m_error = false;
    m_finished = false;
    m_bufferProgress->reset();
    m_totalSize = -1;

    if (m_job) {
        m_job->kill(KJob::Quietly);
    }

    m_job = KIO::get(m_url, KIO::NoReload, KIO::HideProgressInfo);

    connect(m_job, &KIO::TransferJob::data, this, &DragonKioStream::onData);
    connect(m_job, &KJob::result, this, &DragonKioStream::onResult);
    connect(m_job, &KJob::totalAmountChanged, this, &DragonKioStream::onTotalAmountChanged);
    connect(m_job, &KJob::processedAmountChanged, this, &DragonKioStream::onProcessedAmountChanged);
    connect(this, &DragonKioStream::backpressureReleased, m_job, &KJob::resume, Qt::QueuedConnection);

    m_watchdogTimer->start();
}

void DragonKioStream::stop()
{
    m_abort = true;
    m_bufferCv.notify_all();

    if (m_watchdogTimer) {
        m_watchdogTimer->stop();
    }

    if (m_job) {
        disconnect(m_job, nullptr, this, nullptr);
        m_job->kill(KJob::Quietly);
        m_job = nullptr;
    }

    {
        std::scoped_lock lock(m_bufferMutex);
        m_networkBuffer.clear();
        m_bufferOffset = 0;
    }
}

int DragonKioStream::read(std::span<uint8_t> buf, std::stop_token st)
{
    std::unique_lock lock(m_bufferMutex);

    m_bufferCv.wait(lock, st, [this] {
        return !m_networkBuffer.empty() || m_abort.load() || m_error.load() || m_finished.load();
    });

    if (st.stop_requested() || m_abort || m_error) {
        return -1;
    }

    if (m_finished && m_networkBuffer.empty()) {
        return 0;
    }

    int bytesRead = 0;

    while (bytesRead < static_cast<int>(buf.size()) && !m_networkBuffer.empty()) {
        const QByteArray &front = m_networkBuffer.front();
        auto src = std::span(reinterpret_cast<const uint8_t *>(front.constData()), front.size()).subspan(m_bufferOffset);
        auto dst = buf.subspan(bytesRead);
        const int take = static_cast<int>(std::min(src.size(), dst.size()));

        std::ranges::copy(src.first(take), dst.begin());
        bytesRead += take;
        m_bufferOffset += take;

        if (m_bufferOffset >= front.size()) {
            m_networkBuffer.pop_front();
            m_bufferOffset = 0;
        }
    }

    m_streamPosition += bytesRead;

    if (bytesRead > 0) {
        const auto prev = m_bufferDepth.fetch_sub(bytesRead);
        const auto newDepth = prev - bytesRead;
        if (prev > LOW_WATER_MARK && newDepth <= LOW_WATER_MARK) {
            m_isBuffering = true;
            Q_EMIT streamBuffering();
        }
        if (newDepth < MAX_BUFFER_BYTES) {
            releaseBackpressure();
        }
    }

    return bytesRead;
}

void DragonKioStream::applyBackpressure()
{
    // Runs on the KIO job's thread (from onData). Suspending throttles
    // further network delivery so the buffer cannot grow without bound when
    // the consumer (decode thread) is slower than the network.
    if (!m_suspended && m_job && m_bufferDepth.load() >= MAX_BUFFER_BYTES) {
        qCDebug(dragonMediaBackendNetwork) << "KIO buffer full (" << m_bufferDepth.load() << "bytes), suspending job";
        m_suspended = true;
        m_job->suspend();
    }
}

void DragonKioStream::releaseBackpressure()
{
    // May run on the decode thread; emit a queued signal so the job
    // resumes on its own thread.
    if (m_suspended && m_job) {
        m_suspended = false;
        Q_EMIT backpressureReleased();
    }
}

qint64 DragonKioStream::seek(qint64 offset)
{
    stop();

    m_abort = false;
    m_error = false;
    m_finished = false;
    m_bufferOffset = 0;
    m_streamPosition = offset;

    m_job = KIO::get(m_url, KIO::NoReload, KIO::HideProgressInfo);

    m_job->addMetaData(QStringLiteral("resume"), QString::number(offset));

    connect(m_job, &KIO::TransferJob::data, this, &DragonKioStream::onData);
    connect(m_job, &KJob::result, this, &DragonKioStream::onResult);
    connect(m_job, &KJob::totalAmountChanged, this, &DragonKioStream::onTotalAmountChanged);
    connect(m_job, &KJob::processedAmountChanged, this, &DragonKioStream::onProcessedAmountChanged);
    connect(this, &DragonKioStream::backpressureReleased, m_job, &KJob::resume, Qt::QueuedConnection);

    m_watchdogTimer->start();

    return offset;
}

qint64 DragonKioStream::size() const
{
    return m_totalSize.load();
}

qint64 DragonKioStream::position() const
{
    return m_streamPosition.load();
}

DragonBufferProgress *DragonKioStream::bufferProgress() const
{
    return m_bufferProgress;
}

void DragonKioStream::onData(KIO::Job *job, const QByteArray &data)
{
    if (job != m_job || data.isEmpty()) {
        return;
    }

    if (m_watchdogTimer) {
        m_watchdogTimer->start();
    }

    {
        std::scoped_lock lock(m_bufferMutex);
        m_networkBuffer.push_back(data);
    }
    const qint64 newDepth = m_bufferDepth += data.size();
    m_bufferCv.notify_all();

    if (m_isBuffering.exchange(false) && newDepth >= HIGH_WATER_MARK) {
        Q_EMIT streamBuffered();
    }

    applyBackpressure();
}

void DragonKioStream::onResult(KJob *job)
{
    if (job != m_job) {
        return;
    }

    if (m_watchdogTimer) {
        m_watchdogTimer->stop();
    }

    if (job->error()) {
        qCWarning(dragonMediaBackendNetwork) << "KIO error:" << job->errorString();
        m_error = true;
        Q_EMIT errorOccurred(job->errorString());
    } else {
        qCDebug(dragonMediaBackendNetwork) << "KIO job finished successfully";
        m_finished = true;
    }
    m_bufferCv.notify_all();
}

void DragonKioStream::onTotalAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount)
{
    if (job != m_job) {
        return;
    }
    if (unit == KJob::Bytes) {
        m_totalSize = static_cast<qint64>(amount);
    }
}

void DragonKioStream::onProcessedAmountChanged(KJob *job, KJob::Unit unit, qulonglong amount)
{
    if (job != m_job) {
        return;
    }
    if (unit == KJob::Bytes) {
        m_bufferProgress->setBytesReceived(static_cast<qint64>(amount));
    }
}

void DragonKioStream::onWatchdogTimeout()
{
    if (m_abort) {
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
