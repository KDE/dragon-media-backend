/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "httpbodyframer.h"
#include "httpprotocol.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QChronoTimer>
#include <QHttpHeaders>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSslSocket>
#include <QUrl>

#include <expected>
#include <functional>
#include <optional>
#include <variant>

/**
 * Drives a radio stream connection: one GET on a QSslSocket of its own
 * (operating in encrypted or unencrypted mode), through whichever proxy the URL
 * resolves to, following redirects, and with a stall timer on the transport.
 * Response headers are reported as a QHttpHeaders, body bytes arrive framed away,
 * and the end of the stream or a failure finishes the socket.
 */
class DRAGONMEDIABACKEND_EXPORT HttpSocket : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(HttpSocket)

public:
    using Failure = HttpProtocol::Failure;

    struct InformationalResponse {
        bool operator==(const InformationalResponse &) const = default;
    };
    struct RedirectResponse {
        QByteArray location;
        bool operator==(const RedirectResponse &) const = default;
    };
    struct BodyResponse {
        QHttpHeaders headers;
    };
    using ParsedResponse = std::variant<InformationalResponse, RedirectResponse, BodyResponse>;

    struct TlsSessionTicket {
        QByteArray ticket;
        QString host;
        int port = 0;

        [[nodiscard]] bool matches(const QUrl &u) const noexcept
        {
            return host == u.host() && port == HttpProtocol::connectPort(u);
        }
    };

    [[nodiscard]] static std::expected<ParsedResponse, Failure> parseResponseHeaders(QByteArrayView headerBlock,
                                                                                     std::optional<qint64> rangeStart = std::nullopt);

    explicit HttpSocket(QObject *parent = nullptr);
    ~HttpSocket() override;

    void setBackpressurePredicate(std::function<bool()> isBufferFull);
    void open(const QUrl &url, std::optional<qint64> rangeStart = std::nullopt);
    void close();

    Q_SLOT void resumeReading();

    [[nodiscard]] bool isActive() const noexcept
    {
        return m_state != State::Finished && m_state != State::Idle;
    }

    [[nodiscard]] QByteArray sessionTicket() const noexcept
    {
        return m_ticket ? m_ticket->ticket : QByteArray{};
    }

    [[nodiscard]] QString sessionTicketHost() const noexcept
    {
        return m_ticket ? m_ticket->host : QString{};
    }

    [[nodiscard]] int sessionTicketPort() const noexcept
    {
        return m_ticket ? m_ticket->port : 0;
    }

    [[nodiscard]] bool matchesSessionTicket(const QUrl &url) const noexcept
    {
        return !m_ticket || m_ticket->matches(url);
    }

    void setSessionTicket(QByteArray ticket, QString host, int port)
    {
        if (ticket.isEmpty()) {
            m_ticket.reset();
        } else {
            m_ticket = TlsSessionTicket{std::move(ticket), std::move(host), port};
        }
    }

    void clearSessionTicket() noexcept
    {
        m_ticket.reset();
    }

Q_SIGNALS:
    void tlsEncrypted();
    void rawBytesReceived(qint64 totalBytes);
    void headersReceived(const QHttpHeaders &headers);
    /** Emitted when framed body audio bytes are available. The view is valid only during slot execution (direct connection). */
    void bodyDataAvailable(QByteArrayView audioBytes);
    void finished();
    void failed(QNetworkReply::NetworkError error, const QString &message, bool retryable);

private:
    enum class State {
        Idle,
        Connecting,
        AwaitingTls,
        ReadingHeaders,
        ReadingBody,
        Finished,
    };

    void connectToCurrentUrl();
    void prepareSocket(QSslSocket *socket);
    void connectInClear();
    void connectSecurely();
    void sendRequest();
    void handleReadyRead();
    void appendIncoming();
    void ingestFramedChunk(QByteArrayView data);
    [[nodiscard]] std::expected<void, Failure> followRedirect(QByteArrayView location);
    void endStream();
    void fail(const Failure &failure);
    void fail(QNetworkReply::NetworkError error, const QString &message);
    void handleRemoteClosed(const QString &errorText = {});
    void onTransportReady();
    void restartStallTimer();

    static constexpr qint64 MAX_SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;
    static constexpr int MAX_HEADER_BYTES = 64 * 1024;
    static constexpr int MAX_REDIRECTS = 5;

    QUrl m_initialUrl;
    QUrl m_currentUrl;
    std::optional<qint64> m_rangeStart{std::nullopt};
    int m_redirects = 0;

    QPointer<QSslSocket> m_socket;
    QChronoTimer m_stallTimer;
    State m_state = State::Idle;

    // Cleartext through an HTTP forwarding proxy is spoken directly, because
    // the socket engine only knows CONNECT and a raw socket cannot carry the
    // whole URL. https and SOCKS are left to Qt's engine.
    QNetworkProxy m_forwardingProxy;
    bool m_viaForwardingProxy = false;

    /** Response bytes that do not yet form a complete header block. */
    QByteArray m_pendingBytes;
    qint64 m_bytesReceived = 0;
    HttpBodyFramer m_framer;
    std::function<bool()> m_isBufferFull;

    std::optional<TlsSessionTicket> m_ticket;

    friend class TestRadioStream;
};
