/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonhttpsocket.h"
#include "dragonhttpprotocol.h"

#include "dragonmediabackend_network_logging.h"

#include <KLocalizedString>
#include <QSslConfiguration>
#include <QSslSocket>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{
constexpr auto socketStallTimeout = 15s;
}

DragonHttpSocket::DragonHttpSocket(QObject *parent)
    : QObject(parent)
{
    m_stallTimer.setSingleShot(true);
    m_stallTimer.setInterval(socketStallTimeout);
    connect(&m_stallTimer, &QChronoTimer::timeout, this, [this]() {
        fail(QNetworkReply::TimeoutError, i18n("Stream stopped sending data"));
    });
}

DragonHttpSocket::~DragonHttpSocket()
{
    close();
}

void DragonHttpSocket::setBackpressurePredicate(std::function<bool()> isBufferFull)
{
    m_isBufferFull = std::move(isBufferFull);
}

void DragonHttpSocket::open(const QUrl &url, std::optional<qint64> rangeStart)
{
    close();

    if (m_ticket && !m_ticket->matches(url)) {
        clearSessionTicket();
    }

    m_initialUrl = url;
    m_currentUrl = url;
    m_rangeStart = rangeStart;
    m_redirects = 0;

    connectToCurrentUrl();
}

void DragonHttpSocket::close()
{
    m_stallTimer.stop();
    m_state = State::Idle;
    m_pendingBytes.clear();
    m_framer.reset();

    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void DragonHttpSocket::resumeReading()
{
    handleReadyRead();
}

void DragonHttpSocket::connectToCurrentUrl()
{
    m_pendingBytes.clear();
    m_bytesReceived = 0;
    m_state = State::Connecting;

    if (!m_currentUrl.isValid() || m_currentUrl.host().isEmpty()) {
        fail(QNetworkReply::ProtocolUnknownError, i18n("Invalid stream URL: %1", m_currentUrl.toString()));
        return;
    }

    if (DragonHttpProtocol::isHttps(m_currentUrl)) {
        connectSecurely();
    } else {
        connectInClear();
    }
}

void DragonHttpSocket::handleRemoteClosed(const QString &errorText)
{
    if (m_state == State::ReadingBody) {
        endStream();
    } else if (isActive()) {
        fail(QNetworkReply::RemoteHostClosedError, errorText.isEmpty() ? i18n("Connection closed") : errorText);
    }
}

void DragonHttpSocket::onTransportReady()
{
    if (m_state != State::Connecting && m_state != State::AwaitingTls) {
        return;
    }
    m_state = State::ReadingHeaders;
    sendRequest();
}

void DragonHttpSocket::prepareSocket(QSslSocket *socket)
{
    m_socket = socket;

    const QNetworkProxy proxy = DragonHttpProtocol::resolveProxy(m_currentUrl);
    m_viaForwardingProxy = !DragonHttpProtocol::isHttps(m_currentUrl) && DragonHttpProtocol::isForwardingProxy(proxy);
    m_forwardingProxy = m_viaForwardingProxy ? proxy : QNetworkProxy(QNetworkProxy::NoProxy);

    // we handle the http forwarding proxies in this class
    socket->setProxy(m_viaForwardingProxy ? QNetworkProxy(QNetworkProxy::NoProxy) : proxy);
    socket->setReadBufferSize(MAX_SOCKET_BUFFER_BYTES);
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    connect(socket, &QAbstractSocket::readyRead, this, &DragonHttpSocket::handleReadyRead);
    connect(socket, &QAbstractSocket::disconnected, this, [this]() {
        handleRemoteClosed({});
    });
    connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError socketError) {
        if (!isActive()) {
            return;
        }
        if (socketError == QAbstractSocket::SslHandshakeFailedError) {
            clearSessionTicket();
        }
        const QString text = m_socket ? m_socket->errorString() : QString();
        if (socketError == QAbstractSocket::RemoteHostClosedError) {
            handleRemoteClosed(text);
            return;
        }
        fail(DragonHttpProtocol::mapSocketError(socketError), text);
    });
    connect(socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &) {
        clearSessionTicket();
    });

    m_stallTimer.start();
}

void DragonHttpSocket::connectInClear()
{
    auto *socket = new QSslSocket(this);
    prepareSocket(socket);

    connect(socket, &QAbstractSocket::connected, this, &DragonHttpSocket::onTransportReady);

    // A forwarding proxy is connected to in place of the station.
    const QString host = m_viaForwardingProxy ? m_forwardingProxy.hostName() : m_currentUrl.host();
    const quint16 port = m_viaForwardingProxy ? m_forwardingProxy.port() : quint16(DragonHttpProtocol::connectPort(m_currentUrl));
    socket->connectToHost(host, port);
}

void DragonHttpSocket::connectSecurely()
{
    auto *socket = new QSslSocket(this);
    prepareSocket(socket);

    QSslConfiguration sslConfig = socket->sslConfiguration();
    sslConfig.setAllowedNextProtocols({"http/1.1"_ba});
    sslConfig.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);

    if (m_ticket && m_ticket->matches(m_currentUrl)) {
        sslConfig.setSessionTicket(m_ticket->ticket);
    }
    socket->setSslConfiguration(sslConfig);

    m_state = State::AwaitingTls;

    auto updateSessionTicket = [this, socket]() {
        const QByteArray ticket = socket->sslConfiguration().sessionTicket();
        if (!ticket.isEmpty()) {
            const QString peer = socket->peerName();
            const QString host = peer.isEmpty() ? m_currentUrl.host() : peer;
            const quint16 port = socket->peerPort();
            const int connectPort = port == 0 ? DragonHttpProtocol::connectPort(m_currentUrl) : int(port);
            setSessionTicket(ticket, host, connectPort);
        }
    };

    connect(socket, &QSslSocket::encrypted, this, [this, updateSessionTicket]() {
        updateSessionTicket();
        Q_EMIT tlsEncrypted();
        onTransportReady();
    });
    connect(socket, &QSslSocket::newSessionTicketReceived, this, updateSessionTicket);

    socket->connectToHostEncrypted(m_currentUrl.host(), quint16(DragonHttpProtocol::connectPort(m_currentUrl)));
}

void DragonHttpSocket::sendRequest()
{
    if (!m_socket || m_state != State::ReadingHeaders) {
        return;
    }

    m_socket->write(DragonHttpProtocol::buildStreamGetRequest(m_currentUrl, m_forwardingProxy, m_rangeStart));
    restartStallTimer();
}

void DragonHttpSocket::handleReadyRead()
{
    if (!m_socket || !isActive()) {
        return;
    }

    if (m_state == State::ReadingBody && m_isBufferFull && m_isBufferFull()) {
        // While the consumer is the bottleneck, the server is not stalling
        // us; the socket's own read buffer holds its bytes until read() makes
        // room and queues another delivery.
        m_stallTimer.stop();
        return;
    }

    restartStallTimer();

    // Response headers are read one block at a time, so an informational
    // answer does not swallow the status line that follows it in the same
    // segment. A redirect or a failure changes the state and ends the loop.
    if (m_state == State::ReadingHeaders) {
        appendIncoming();

        while (m_state == State::ReadingHeaders) {
            qsizetype headerEnd = m_pendingBytes.indexOf("\r\n\r\n"_ba);
            qsizetype delimiterLength = 4;
            if (const qsizetype lfLf = m_pendingBytes.indexOf("\n\n"_ba); lfLf >= 0 && (headerEnd < 0 || lfLf < headerEnd)) {
                headerEnd = lfLf;
                delimiterLength = 2;
            }

            if (headerEnd < 0) {
                if (m_pendingBytes.size() > MAX_HEADER_BYTES) {
                    fail(QNetworkReply::ProtocolFailure, i18n("Malformed response"));
                }
                return;
            }

            // The block is taken out before it is parsed, because the parser reads
            // straight out of a buffer that consuming it would move.
            const QByteArray headerBlock = m_pendingBytes.first(headerEnd);
            m_pendingBytes.remove(0, headerEnd + delimiterLength);
            const auto result = parseResponseHeaders(headerBlock, m_rangeStart);
            if (!result) {
                fail(result.error());
                return;
            }

            bool continueReading = false;
            std::visit(Overloaded{
                           [&](const InformationalResponse &) {
                               continueReading = true;
                           },
                           [&](const RedirectResponse &redirect) {
                               if (const auto res = followRedirect(redirect.location); !res) {
                                   fail(res.error());
                               }
                           },
                           [&](const BodyResponse &body) {
                               m_framer.startFromHeaders(body.headers);
                               m_state = State::ReadingBody;
                               Q_EMIT headersReceived(body.headers);
                           },
                       },
                       *result);

            if (continueReading) {
                continue;
            }
            if (m_state != State::ReadingBody) {
                return;
            }
        }
    }

    if (m_state == State::ReadingBody) {
        if (!m_pendingBytes.isEmpty()) {
            const QByteArray initialBody = std::move(m_pendingBytes);
            m_pendingBytes.clear();
            ingestFramedChunk(initialBody);
            if (m_state != State::ReadingBody) {
                return;
            }
        }

        while (m_socket && m_socket->bytesAvailable() > 0 && m_state == State::ReadingBody) {
            if (m_isBufferFull && m_isBufferFull()) {
                m_stallTimer.stop();
                return;
            }
            const QByteArray chunk = m_socket->readAll();
            if (chunk.isEmpty()) {
                break;
            }
            m_bytesReceived += chunk.size();
            Q_EMIT rawBytesReceived(m_bytesReceived);
            ingestFramedChunk(chunk);
        }
    }
}

// Reads everything the socket holds into the pending-bytes accumulator while reading headers.
void DragonHttpSocket::appendIncoming()
{
    if (!m_socket) {
        return;
    }
    const QByteArray incoming = m_socket->readAll();
    if (incoming.isEmpty()) {
        return;
    }
    m_bytesReceived += incoming.size();
    Q_EMIT rawBytesReceived(m_bytesReceived);
    m_pendingBytes += incoming;
}

// Frames the body bytes and hands the audio they carry downstream via direct callback/signal.
void DragonHttpSocket::ingestFramedChunk(QByteArrayView data)
{
    const auto res = m_framer.feed(data, [this](QByteArrayView audio) {
        if (!audio.isEmpty()) {
            Q_EMIT bodyDataAvailable(audio);
        }
    });

    if (!res) {
        fail(QNetworkReply::ProtocolFailure, i18n("Malformed response"));
        return;
    }

    if (m_framer.isComplete() && m_state == State::ReadingBody) {
        endStream();
    }
}

std::expected<DragonHttpSocket::ParsedResponse, DragonHttpSocket::Failure> DragonHttpSocket::parseResponseHeaders(QByteArrayView headerBlock,
                                                                                                                  std::optional<qint64> rangeStart)
{
    auto result = DragonHttpProtocol::parseIcyResponse(headerBlock).transform_error([](const DragonHttpProtocol::IcyParseFailure &err) {
        return Failure{QNetworkReply::ProtocolFailure, err.message()};
    });

    return result.and_then([rangeStart](const DragonHttpProtocol::IcyResponse &response) -> std::expected<ParsedResponse, Failure> {
        const int status = response.statusCode;
        qCDebug(dragonMediaBackendNetwork) << "response headers received, status" << status;

        if (status >= 100 && status < 200) {
            return InformationalResponse{};
        }

        if (status >= 300 && status < 400) {
            const QByteArrayView location = response.headers.value(QHttpHeaders::WellKnownHeader::Location);
            if (location.isEmpty()) {
                return std::unexpected(Failure{DragonHttpProtocol::errorForStatus(status), i18n("Server responded with status %1", status)});
            }
            return RedirectResponse{QByteArray(location)};
        }

        if (status == 407) {
            return std::unexpected(Failure{QNetworkReply::ProxyAuthenticationRequiredError, i18n("Proxy requires authentication")});
        }

        if (status < 200 || status >= 300) {
            return std::unexpected(Failure{DragonHttpProtocol::errorForStatus(status), i18n("Server responded with status %1", status)});
        }

        if (rangeStart) {
            if (status == 206) {
                const auto cr = DragonHttpProtocol::parseContentRange(response.headers.value(QHttpHeaders::WellKnownHeader::ContentRange));
                if (cr && cr->first >= 0 && cr->first != *rangeStart) {
                    return std::unexpected(Failure{QNetworkReply::ProtocolFailure, i18n("Server returned unexpected byte range")});
                }
            } else if (status == 200 && *rangeStart == 0) {
                // A request for bytes=0- answered with 200 OK contains the whole body starting from byte 0.
            } else {
                return std::unexpected(Failure{QNetworkReply::ProtocolFailure, i18n("Server ignored requested byte range")});
            }
        }

        return BodyResponse{response.headers};
    });
}

std::expected<void, DragonHttpSocket::Failure> DragonHttpSocket::followRedirect(QByteArrayView location)
{
    if (location.isEmpty()) {
        return std::unexpected(Failure{QNetworkReply::ProtocolFailure, i18n("Unusable redirect address")});
    }
    if (++m_redirects > MAX_REDIRECTS) {
        return std::unexpected(Failure{QNetworkReply::TooManyRedirectsError, i18n("Too many redirects")});
    }

    const QUrl target = QUrl::fromEncoded(location);
    const QUrl resolved = m_currentUrl.resolved(target);

    if (!resolved.isValid() || resolved.host().isEmpty()) {
        return std::unexpected(Failure{QNetworkReply::ProtocolFailure, i18n("Unusable redirect address")});
    }
    const QString scheme = resolved.scheme();
    if (scheme != "http"_L1 && scheme != "https"_L1) {
        return std::unexpected(Failure{QNetworkReply::ProtocolFailure, i18n("Unusable redirect address")});
    }
    if (DragonHttpProtocol::isHttps(m_initialUrl) && !DragonHttpProtocol::isHttps(resolved)) {
        return std::unexpected(Failure{QNetworkReply::InsecureRedirectError, i18n("Unusable redirect address")});
    }

    m_currentUrl = resolved;
    qCDebug(dragonMediaBackendNetwork) << "following redirect to" << m_currentUrl.toString();
    close();
    connectToCurrentUrl();
    return {};
}

void DragonHttpSocket::endStream()
{
    if (m_state != State::ReadingBody) {
        return;
    }

    // Whatever the transport still holds belongs to the audio buffer; hand it
    // over regardless of the cap, otherwise data held back by backpressure
    // would be lost. A body without framing ends at the hang-up rather than
    // at a declared length, so its last bytes arrive this way.
    if (!m_framer.isComplete()) {
        if (!m_pendingBytes.isEmpty()) {
            const QByteArray initialBody = std::move(m_pendingBytes);
            m_pendingBytes.clear();
            ingestFramedChunk(initialBody);
            if (m_state != State::ReadingBody) {
                return;
            }
        }
        if (m_socket && m_socket->bytesAvailable() > 0) {
            const QByteArray remaining = m_socket->readAll();
            m_bytesReceived += remaining.size();
            Q_EMIT rawBytesReceived(m_bytesReceived);
            ingestFramedChunk(remaining);
            if (m_state != State::ReadingBody) {
                return;
            }
        }
    }

    if (m_framer.isTruncated()) {
        fail(QNetworkReply::RemoteHostClosedError, i18n("Connection closed"));
        return;
    }

    close();

    Q_EMIT finished();
}

void DragonHttpSocket::fail(const Failure &failure)
{
    fail(failure.error, failure.message);
}

void DragonHttpSocket::fail(QNetworkReply::NetworkError error, const QString &message)
{
    if (!isActive()) {
        return;
    }

    const bool retryable = DragonHttpProtocol::isRetryable(error);
    qCWarning(dragonMediaBackendNetwork) << "error:" << error << message;

    close();

    Q_EMIT failed(error, message, retryable);
}

void DragonHttpSocket::restartStallTimer()
{
    if (isActive()) {
        m_stallTimer.start();
    }
}
