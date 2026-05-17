/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonkiostream.h"

#include "dragonbufferprogress.h"
#include "dragonsdl_network_logging.h"

#include <KIO/TransferJob>
#include <QUrl>

#include <cstring>

DragonKioStream::DragonKioStream(QObject *parent)
    : QObject(parent)
    , m_bufferProgress(new DragonBufferProgress(this))
{
}

DragonKioStream::~DragonKioStream()
{
    stop();
}

void DragonKioStream::setUrl(const QUrl &url)
{
    m_url = url;
}

void DragonKioStream::start()
{
    qCDebug(dragonsdlNetwork) << "DragonKioStream start" << m_url.toString();

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
}

void DragonKioStream::stop()
{
    m_abort = true;
    m_bufferCv.notify_all();

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
    int toRead = static_cast<int>(buf.size());

    while (bytesRead < toRead && !m_networkBuffer.empty()) {
        const QByteArray &front = m_networkBuffer.front();
        int available = front.size() - m_bufferOffset;
        int take = std::min(toRead - bytesRead, available);

        std::memcpy(buf.data() + bytesRead, front.constData() + m_bufferOffset, take);
        bytesRead += take;
        m_bufferOffset += take;

        if (m_bufferOffset >= front.size()) {
            m_networkBuffer.pop_front();
            m_bufferOffset = 0;
        }
    }

    m_streamPosition += bytesRead;
    return bytesRead;
}

int64_t DragonKioStream::seek(int64_t offset)
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

    {
        std::scoped_lock lock(m_bufferMutex);
        m_networkBuffer.push_back(data);
    }
    m_bufferCv.notify_all();
}

void DragonKioStream::onResult(KJob *job)
{
    if (job != m_job) {
        return;
    }

    if (job->error()) {
        qCWarning(dragonsdlNetwork) << "KIO error:" << job->errorString();
        m_error = true;
        Q_EMIT errorOccurred(job->errorString());
    } else {
        qCDebug(dragonsdlNetwork) << "KIO job finished successfully";
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
