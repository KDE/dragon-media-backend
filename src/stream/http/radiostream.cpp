/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "radiostream.h"

#include "httpprotocol.h"
#include "httpsocket.h"

#include "stream/bufferprogress.h"

#include "dragonmediabackend_network_logging.h"
#include <KLocalizedString>
#include <QNetworkReply>
#include <QUrl>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{
constexpr auto reconnectDelay = 2s;
}

RadioStream::RadioStream(QObject *parent)
    : Stream(parent)
    , m_httpSocket(new HttpSocket(this))
    , m_buffer(this, {.highWaterMark = StreamBuffer::RADIO_HIGH_WATER_MARK})
{
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(reconnectDelay);
    connect(&m_reconnectTimer, &QChronoTimer::timeout, this, [this]() {
        if (!m_buffer.isAborted()) {
            restart();
        }
    });

    m_httpSocket->setBackpressurePredicate([this]() {
        return isBufferFull();
    });

    m_buffer.setBackpressureRelievedCallback([this]() {
        QMetaObject::invokeMethod(m_httpSocket, &HttpSocket::resumeReading, Qt::QueuedConnection);
    });

    connect(m_httpSocket, &HttpSocket::tlsEncrypted, this, [this]() {
        m_buffer.bufferProgress()->setTlsHandshakeComplete(true);
    });
    connect(m_httpSocket, &HttpSocket::rawBytesReceived, this, [this](qint64 totalBytes) {
        m_buffer.bufferProgress()->setBytesReceived(totalBytes);
    });
    connect(m_httpSocket, &HttpSocket::headersReceived, this, &RadioStream::applyResponseHeaders);
    connect(m_httpSocket, &HttpSocket::bodyDataAvailable, this, &RadioStream::ingestBody, Qt::DirectConnection);
    connect(m_httpSocket, &HttpSocket::finished, this, &RadioStream::handleTransportFinished);
    connect(m_httpSocket, &HttpSocket::failed, this, &RadioStream::handleTransportFailed);
}

RadioStream::~RadioStream()
{
    stop();
}

void RadioStream::setUrl(const QUrl &url)
{
    if (m_url != url && m_httpSocket) {
        if (!m_httpSocket->matchesSessionTicket(url)) {
            m_httpSocket->clearSessionTicket();
        }
    }
    m_url = url;
}

void RadioStream::start()
{
    qCDebug(dragonMediaBackendNetwork) << "start" << m_url.toString();

    m_buffer.reset();
    m_icyDemuxer.reset();
    m_buffer.setTotalSize(-1);
    m_buffer.setPosition(0);
    if (m_acceptsRanges.exchange(false)) {
        Q_EMIT seekableChanged(false);
    }

    openRequest(std::nullopt);
}

void RadioStream::prepareForNewTransport()
{
    m_buffer.reset();
    m_icyDemuxer.clearPendingBytes();
}

void RadioStream::openRequest(std::optional<qint64> rangeStart)
{
    // Stopping the timer here is what keeps a reconnect scheduled for the
    // previous connection from restarting under a stream that stop() and
    // start() have already replaced.
    m_reconnectTimer.stop();
    m_httpSocket->open(m_url, rangeStart);
}

void RadioStream::restart()
{
    const qint64 resumePos = m_buffer.position();
    if (m_acceptsRanges.load() && resumePos > 0) {
        prepareForNewTransport();
        m_buffer.setPosition(resumePos);
        openRequest(resumePos);
    } else {
        start();
    }
}

void RadioStream::stop()
{
    m_buffer.abort();

    m_reconnectTimer.stop();
    m_httpSocket->close();
}

int RadioStream::read(std::span<uint8_t> buf, const std::stop_token &st)
{
    return m_buffer.read(buf, st);
}

qint64 RadioStream::seek(qint64 offset)
{
    if (!m_acceptsRanges.load()) {
        return -1;
    }

    qCDebug(dragonMediaBackendNetwork) << "seek to byte offset" << offset;

    stop();

    prepareForNewTransport();
    m_buffer.setPosition(offset);

    openRequest(offset);

    return offset;
}

bool RadioStream::isBufferFull() const
{
    return m_buffer.isFull();
}

void RadioStream::applyResponseHeaders(const QHttpHeaders &headers)
{
    // The budget refreshes here rather than when bytes arrive, so a reconnect
    // that reaches even a stalling server is worth one.
    m_reconnectAttempts = 0;

    // if we got here without setting TLS handshake complete, it's an HTTP connection
    m_buffer.bufferProgress()->setTlsHandshakeComplete(true);
    m_buffer.bufferProgress()->setHeadersReceived(true);

    if (const qint64 metaint = headers.intValue("icy-metaint"_ba).value_or(0); metaint > 0) {
        m_icyDemuxer.setIcyMetaInt(metaint);
        qCDebug(dragonMediaBackendNetwork) << "ICY metadata interval:" << metaint;
    }

    const auto contentRange = HttpProtocol::parseContentRange(headers.value(QHttpHeaders::WellKnownHeader::ContentRange));
    if (contentRange && contentRange->total > 0) {
        m_buffer.setTotalSize(contentRange->total);
        qCDebug(dragonMediaBackendNetwork) << "Total size from Content-Range:" << contentRange->total;
    } else if (const qint64 contentLength = headers.intValue(QHttpHeaders::WellKnownHeader::ContentLength).value_or(0); contentLength > 0) {
        m_buffer.setTotalSize(contentLength);
        qCDebug(dragonMediaBackendNetwork) << "Total size from Content-Length:" << contentLength;
    }

    if (headers.value(QHttpHeaders::WellKnownHeader::AcceptRanges).compare("bytes"_ba, Qt::CaseInsensitive) == 0) {
        if (!m_acceptsRanges.exchange(true)) {
            qCDebug(dragonMediaBackendNetwork) << "Server supports Range requests";
            Q_EMIT seekableChanged(true);
        }
    }
}

void RadioStream::ingestBody(QByteArrayView data)
{
    m_icyDemuxer.feed(
        data,
        [this](QByteArrayView audio) {
            addToAudioBuffer(audio);
        },
        [this](const DragonIcyMetadata &metadata) {
            Q_EMIT metadataReady(metadata);
        });
}

void RadioStream::handleTransportFinished()
{
    if (m_buffer.isAborted()) {
        return;
    }

    qCDebug(dragonMediaBackendNetwork) << "stream ended by the server";
    m_buffer.setFinished();
}

void RadioStream::handleTransportFailed(QNetworkReply::NetworkError error, const QString &message, bool retryable)
{
    qCDebug(dragonMediaBackendNetwork) << "transport failed" << error << "retryable" << retryable;

    // A failure is a stall while the reconnect budget lasts: the player keeps
    // the stream instead of giving up on it, and read() waits for the fresh
    // connection instead of failing. Once the budget is spent the error is
    // reported and playback really is over.
    if (retryable && m_reconnectAttempts < MAX_RECONNECTS) {
        ++m_reconnectAttempts;
        if (m_buffer.isAborted()) {
            return;
        }
        m_reconnectTimer.start();
        Q_EMIT streamStalled();
        if (m_buffer.isAborted()) {
            m_reconnectTimer.stop();
            return;
        }
        if (m_reconnectTimer.isActive()) {
            qCDebug(dragonMediaBackendNetwork) << "connection failed, will reconnect";
        }
        return;
    }

    if (retryable) {
        qCWarning(dragonMediaBackendNetwork) << "giving up on the stream after" << (m_reconnectAttempts) << "reconnects";
    }
    reportStreamError(message);
}

void RadioStream::reportStreamError(const QString &message)
{
    if (m_buffer.isAborted()) {
        return;
    }

    m_buffer.setError();
    Q_EMIT errorOccurred(i18n("Network error: %1", message));
}

void RadioStream::addToAudioBuffer(QByteArrayView data)
{
    m_buffer.write(data);
}

void RadioStream::processMetadata(QByteArrayView metadata)
{
    if (const auto icy = m_icyDemuxer.processBlock(metadata)) {
        Q_EMIT metadataReady(*icy);
    }
}
