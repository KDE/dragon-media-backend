/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Mock live-audio server for DragonRadioStream tests. Answers either as a
 * modern HTTP origin or as SHOUTcast DNAS, so the same expectations can be
 * checked against whichever transport the stream had to use.
 */

#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

class StreamTestServer : public QObject
{
public:
    enum class Dialect {
        Http,
        Shoutcast,
    };

    explicit StreamTestServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
    {
        connect(m_server, &QTcpServer::newConnection, this, &StreamTestServer::onNewConnection);
    }

    bool start()
    {
        if (!m_server->listen(QHostAddress::LocalHost, 0)) {
            qWarning() << "Failed to start stream server:" << m_server->errorString();
            return false;
        }
        return true;
    }

    void stop()
    {
        m_server->close();
        hangUp();
    }

    [[nodiscard]] quint16 port() const
    {
        return m_server->serverPort();
    }

    [[nodiscard]] QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/stream").arg(port()));
    }

    void setDialect(Dialect dialect)
    {
        m_dialect = dialect;
    }

    void setStatus(int code, const QByteArray &reason)
    {
        m_statusCode = code;
        m_reason = reason;
    }

    void setBody(const QByteArray &body)
    {
        m_body = body;
    }

    /** Interleaves a metadata block every metaint audio bytes, as ICY does. */
    void setIcyMetadata(int metaint, const QByteArray &streamTitle)
    {
        m_metaint = metaint;
        m_streamTitle = streamTitle;
    }

    void setHeader(const QByteArray &name, const QByteArray &value)
    {
        m_headers.append(qMakePair(name, value));
    }

    /** Live stations keep the connection open; a file server does not. */
    void setCloseAfterBody(bool close)
    {
        m_closeAfterBody = close;
    }

    /** Answer once with a redirect, the way a station directory or a short link does. */
    void setRedirect(const QByteArray &location)
    {
        m_redirect = location;
        m_redirectPending = true;
    }

    /** A framed body ends at its length even while the connection stays open. */
    void setContentLengthHeader(bool send)
    {
        m_contentLength = send;
    }

    /** As a proxy: demand Basic authentication, the way an enforced gateway does. */
    void setProxyCredentials(const QByteArray &user, const QByteArray &password)
    {
        m_proxyUser = user;
        m_proxyPassword = password;
    }

    /** Act as a SOCKS5 proxy server. */
    void setSocksProxy(bool enable)
    {
        m_isSocksProxy = enable;
    }

    [[nodiscard]] QByteArray lastSocksHost() const
    {
        return m_lastSocksHost;
    }

    [[nodiscard]] int lastSocksPort() const
    {
        return m_lastSocksPort;
    }

    /** Chunked transfer encoding, which only Qt's own HTTP stack can read. */
    void setChunkedBody(bool chunked)
    {
        m_chunked = chunked;
    }

    /**
     * Trailer header lines, each CRLF-terminated, sent after the final chunk;
     * the empty line that ends the trailer section is written for you.
     */
    void setChunkedTrailer(const QByteArray &trailer)
    {
        m_chunkedTrailer = trailer;
    }

    /** Ends every live connection: how a station stops sending. */
    void hangUp()
    {
        const QList<QTcpSocket *> clients = m_clients;
        for (QTcpSocket *socket : clients) {
            socket->disconnectFromHost();
        }
    }

    [[nodiscard]] QByteArray lastRequest() const
    {
        return m_lastRequest;
    }

    /**
     * How many times a client has asked for the stream, which is how a test can
     * tell whether the stream had to retry the request on its own socket.
     */
    [[nodiscard]] int connectionCount() const
    {
        return m_connections;
    }

private:
    enum class SocksStage {
        Greeting,
        Command,
        Done,
    };

    struct ClientState {
        QByteArray buffer;
        SocksStage socksStage = SocksStage::Greeting;
    };

    bool processSocksHandshake(QTcpSocket *socket, ClientState *state)
    {
        if (state->socksStage == SocksStage::Greeting) {
            if (state->buffer.size() < 2) {
                return false;
            }
            const int nmethods = static_cast<uchar>(state->buffer[1]);
            if (state->buffer.size() < 2 + nmethods) {
                return false;
            }
            if (state->buffer[0] != 5) {
                socket->disconnectFromHost();
                return false;
            }
            state->buffer.remove(0, 2 + nmethods);
            socket->write("\x05\x00", 2);
            socket->flush();
            state->socksStage = SocksStage::Command;
        }

        if (state->socksStage == SocksStage::Command) {
            if (state->buffer.size() < 4) {
                return false;
            }
            const int atyp = static_cast<uchar>(state->buffer[3]);
            int addrLen = 0;
            if (atyp == 1) {
                addrLen = 4;
            } else if (atyp == 3) {
                if (state->buffer.size() < 5) {
                    return false;
                }
                addrLen = 1 + static_cast<uchar>(state->buffer[4]);
            } else if (atyp == 4) {
                addrLen = 16;
            } else {
                socket->disconnectFromHost();
                return false;
            }
            const int totalCmdLen = 4 + addrLen + 2;
            if (state->buffer.size() < totalCmdLen) {
                return false;
            }
            if (atyp == 3) {
                const int dlen = static_cast<uchar>(state->buffer[4]);
                m_lastSocksHost = state->buffer.mid(5, dlen);
                m_lastSocksPort = (static_cast<uchar>(state->buffer[5 + dlen]) << 8) | static_cast<uchar>(state->buffer[6 + dlen]);
            } else if (atyp == 1) {
                m_lastSocksHost = QHostAddress(qFromBigEndian<quint32>(state->buffer.constData() + 4)).toString().toUtf8();
                m_lastSocksPort = (static_cast<uchar>(state->buffer[8]) << 8) | static_cast<uchar>(state->buffer[9]);
            }
            state->buffer.remove(0, totalCmdLen);
            socket->write("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10);
            socket->flush();
            state->socksStage = SocksStage::Done;
        }

        return state->socksStage == SocksStage::Done;
    }

    void onNewConnection()
    {
        while (m_server->hasPendingConnections()) {
            QTcpSocket *socket = m_server->nextPendingConnection();
            m_clients.append(socket);
            auto state = std::make_shared<ClientState>();
            if (!m_isSocksProxy) {
                state->socksStage = SocksStage::Done;
            }
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, state]() {
                state->buffer.append(socket->readAll());

                if (state->socksStage != SocksStage::Done && !processSocksHandshake(socket, state.get())) {
                    return;
                }

                if (!state->buffer.contains("\r\n\r\n")) {
                    return;
                }
                ++m_connections;
                const qsizetype headerEnd = state->buffer.indexOf("\r\n\r\n");
                m_lastRequest = state->buffer.left(headerEnd + 4);
                state->buffer.remove(0, headerEnd + 4);
                respond(socket);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                m_clients.removeAll(socket);
                socket->deleteLater();
            });
        }
    }

    void respond(QTcpSocket *socket)
    {
        if (!proxyAuthorized()) {
            socket->write("HTTP/1.1 407 Proxy Authentication Required\r\n");
            socket->write("Proxy-Authenticate: Basic realm=\"stream test\"\r\n");
            socket->write("Content-Length: 0\r\n\r\n");
            socket->flush();
            socket->disconnectFromHost();
            return;
        }

        QByteArray statusLine;
        if (m_dialect == Dialect::Shoutcast) {
            // SHOUTcast DNAS 1.x: the pre-standard spelling of "HTTP/1.0 200 OK".
            statusLine = QByteArrayLiteral("ICY ") + QByteArray::number(m_statusCode) + ' ' + m_reason;
        } else {
            statusLine = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(m_statusCode) + ' ' + m_reason;
        }

        if (m_redirectPending) {
            m_redirectPending = false;
            const QByteArray redirectLine = m_dialect == Dialect::Shoutcast ? QByteArrayLiteral("ICY 302 Found") : QByteArrayLiteral("HTTP/1.1 302 Found");
            QByteArray response = redirectLine + QByteArrayLiteral("\r\n");
            response += QByteArrayLiteral("Location: ") + m_redirect + QByteArrayLiteral("\r\n");
            response += QByteArrayLiteral("Content-Length: 0\r\n");
            response += QByteArrayLiteral("\r\n");
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            return;
        }

        const QByteArray body = payload();
        const bool framed = m_contentLength.value_or(m_closeAfterBody);

        QByteArray response = statusLine + QByteArrayLiteral("\r\n");
        response += QByteArrayLiteral("Content-Type: ") + m_contentType + QByteArrayLiteral("\r\n");
        if (m_metaint > 0) {
            response += QByteArrayLiteral("icy-metaint: ") + QByteArray::number(m_metaint) + QByteArrayLiteral("\r\n");
        }
        for (const auto &[name, value] : m_headers) {
            response += name + QByteArrayLiteral(": ") + value + QByteArrayLiteral("\r\n");
        }
        if (m_chunked) {
            response += QByteArrayLiteral("Transfer-Encoding: chunked\r\n");
        } else if (framed) {
            response += QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n");
        }
        if (m_dialect == Dialect::Http) {
            response += QByteArrayLiteral("Connection: close\r\n");
        }
        response += QByteArrayLiteral("\r\n");

        socket->write(response);
        socket->write(m_chunked ? chunked(body) : body);
        socket->flush();

        if (m_closeAfterBody) {
            socket->disconnectFromHost();
        }
    }

    /** Whether a client asking through us as a proxy proved itself. */
    [[nodiscard]] bool proxyAuthorized() const
    {
        if (m_proxyUser.isEmpty()) {
            return true;
        }

        QByteArray credentials = m_proxyUser;
        credentials.append(':');
        credentials.append(m_proxyPassword);

        QByteArray expected = QByteArrayLiteral("Proxy-Authorization: Basic ");
        expected.append(credentials.toBase64());
        return m_lastRequest.contains(expected);
    }

    /** The audio, with ICY metadata interleaved when the test asked for it. */
    [[nodiscard]] QByteArray payload() const
    {
        if (m_metaint <= 0) {
            return m_body;
        }

        QByteArray out;
        QByteArray titleMetadata = "StreamTitle='" + m_streamTitle + "';";
        const int titleBlocks = (titleMetadata.size() + 15) / 16;
        titleMetadata.append(QByteArray(titleBlocks * 16 - titleMetadata.size(), '\0'));

        for (qsizetype offset = 0; offset < m_body.size(); offset += m_metaint) {
            const qsizetype chunk = std::min<qsizetype>(m_metaint, m_body.size() - offset);
            out.append(m_body.mid(offset, chunk));

            // The first block announces the title; later ones say nothing changed.
            if (offset == 0) {
                out.append(static_cast<char>(titleBlocks));
                out.append(titleMetadata);
            } else {
                out.append(char(0));
            }
        }
        return out;
    }

    [[nodiscard]] QByteArray chunked(const QByteArray &body) const
    {
        static constexpr qsizetype chunkSize = 2048;

        QByteArray out;
        for (qsizetype offset = 0; offset < body.size(); offset += chunkSize) {
            const QByteArray chunk = body.mid(offset, chunkSize);
            out += QByteArray::number(chunk.size(), 16) + QByteArrayLiteral("\r\n") + chunk + QByteArrayLiteral("\r\n");
        }
        out += QByteArrayLiteral("0\r\n") + m_chunkedTrailer + QByteArrayLiteral("\r\n");
        return out;
    }

    QTcpServer *m_server = nullptr;
    QList<QTcpSocket *> m_clients;

    Dialect m_dialect = Dialect::Http;
    int m_statusCode = 200;
    QByteArray m_reason = QByteArrayLiteral("OK");
    QByteArray m_body;
    QByteArray m_contentType = QByteArrayLiteral("audio/mpeg");
    QList<QPair<QByteArray, QByteArray>> m_headers;
    int m_metaint = 0;
    QByteArray m_streamTitle;
    bool m_closeAfterBody = true;
    bool m_chunked = false;
    QByteArray m_chunkedTrailer;
    std::optional<bool> m_contentLength;
    QByteArray m_redirect;
    bool m_redirectPending = false;
    QByteArray m_proxyUser;
    QByteArray m_proxyPassword;
    bool m_isSocksProxy = false;
    QByteArray m_lastSocksHost;
    int m_lastSocksPort = 0;

    QByteArray m_lastRequest;
    int m_connections = 0;
};