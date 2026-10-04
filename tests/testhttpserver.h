/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Shared mock HTTP server for Dragon Multimedia tests.
 * Supports Range requests so KIO seek via resume metadata works correctly.
 */

#pragma once

#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

class TestHttpServer : public QObject
{
public:
    explicit TestHttpServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
    {
        connect(m_server, &QTcpServer::newConnection, this, &TestHttpServer::onNewConnection);
    }

    bool start(quint16 port = 0)
    {
        if (!m_server->listen(QHostAddress::LocalHost, port)) {
            qWarning() << "Failed to start HTTP server:" << m_server->errorString();
            return false;
        }
        m_port = m_server->serverPort();
        qDebug() << "HTTP server listening on port" << m_port;
        return true;
    }

    void stop()
    {
        m_server->close();
        for (QTcpSocket *socket : m_pendingClients) {
            socket->close();
        }
        m_pendingClients.clear();
    }

    [[nodiscard]] quint16 port() const
    {
        return m_port;
    }

    void serveFile(const QString &path, const QByteArray &contentType = QByteArrayLiteral("application/octet-stream"))
    {
        m_filePath = path;
        m_contentType = contentType;
    }

    void setAcceptRanges(bool accept)
    {
        m_acceptRanges = accept;
    }

    void setThrottle(int chunkBytes, int intervalMs)
    {
        m_throttleChunkBytes = chunkBytes;
        m_throttleIntervalMs = intervalMs;
    }

    void clearThrottle()
    {
        m_throttleChunkBytes = 0;
    }

    [[nodiscard]] bool acceptRanges() const
    {
        return m_acceptRanges;
    }

private:
    void onNewConnection()
    {
        while (m_server->hasPendingConnections()) {
            QTcpSocket *socket = m_server->nextPendingConnection();
            m_pendingClients.append(socket);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                handleRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                m_pendingClients.removeAll(socket);
                socket->deleteLater();
            });
        }
    }

    void handleRequest(QTcpSocket *socket)
    {
        QByteArray request = socket->readAll();
        if (!request.startsWith("GET ")) {
            sendError(socket, 400, "Bad Request");
            return;
        }

        QFile file(m_filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            sendError(socket, 404, "Not Found");
            return;
        }

        QByteArray content = file.readAll();
        file.close();
        qint64 totalSize = content.size();

        qint64 rangeStart = 0;
        qint64 rangeEnd = totalSize - 1;
        bool hasRange = false;

        if (m_acceptRanges) {
            int rangeIdx = request.indexOf("Range: bytes=");
            if (rangeIdx >= 0) {
                int valueStart = rangeIdx + 13;
                int lineEnd = request.indexOf("\r\n", valueStart);
                QByteArray rangeValue = request.mid(valueStart, lineEnd - valueStart);
                int dashIdx = rangeValue.indexOf('-');
                if (dashIdx >= 0) {
                    QByteArray startStr = rangeValue.left(dashIdx);
                    QByteArray endStr = rangeValue.mid(dashIdx + 1);
                    bool ok1 = false, ok2 = false;
                    if (!startStr.isEmpty()) {
                        rangeStart = startStr.toLongLong(&ok1);
                    }
                    if (!endStr.isEmpty()) {
                        rangeEnd = endStr.toLongLong(&ok2);
                    }
                    if (ok1 || ok2) {
                        hasRange = true;
                        if (!ok2 || rangeEnd >= totalSize) {
                            rangeEnd = totalSize - 1;
                        }
                        if (rangeStart < 0)
                            rangeStart = 0;
                    }
                }
            }
        }

        QByteArray response;
        qint64 sendOffset = 0;
        qint64 sendLength = totalSize;

        if (hasRange) {
            if (rangeStart >= totalSize) {
                sendError(socket, 416, "Range Not Satisfiable");
                return;
            }
            sendOffset = rangeStart;
            sendLength = rangeEnd - rangeStart + 1;
            response.append("HTTP/1.1 206 Partial Content\r\n");
            response.append("Content-Type: " + m_contentType + "\r\n");
            response.append("Content-Length: " + QByteArray::number(sendLength) + "\r\n");
            response.append("Content-Range: bytes " + QByteArray::number(rangeStart) + "-" + QByteArray::number(rangeEnd) + "/" + QByteArray::number(totalSize)
                            + "\r\n");
            response.append("Connection: close\r\n");
            response.append("\r\n");
        } else {
            response.append("HTTP/1.1 200 OK\r\n");
            response.append("Content-Type: " + m_contentType + "\r\n");
            response.append("Content-Length: " + QByteArray::number(totalSize) + "\r\n");
            if (m_acceptRanges) {
                response.append("Accept-Ranges: bytes\r\n");
            }
            response.append("Connection: close\r\n");
            response.append("\r\n");
        }

        if (m_throttleChunkBytes > 0) {
            sendThrottled(socket, response, content, sendOffset, sendLength);
        } else {
            socket->write(response);
            socket->write(content.mid(sendOffset, sendLength));
            socket->flush();
            socket->disconnectFromHost();
        }
    }

    void sendThrottled(QTcpSocket *socket, const QByteArray &response, const QByteArray &content, qint64 sendOffset, qint64 sendLength)
    {
        socket->write(response);

        const int chunk = m_throttleChunkBytes;
        auto *timer = new QTimer(socket);
        timer->setInterval(m_throttleIntervalMs);
        qint64 sent = 0;

        QObject::connect(timer, &QTimer::timeout, socket, [socket, timer, content, sendOffset, sendLength, chunk, sent]() mutable {
            const qint64 remaining = sendLength - sent;
            const qint64 now = qMin<qint64>(chunk, remaining);
            socket->write(content.mid(sendOffset + sent, now));
            sent += now;
            if (sent >= sendLength) {
                timer->stop();
                socket->flush();
                socket->disconnectFromHost();
            }
        });

        const qint64 first = qMin<qint64>(chunk, sendLength);
        if (first > 0) {
            socket->write(content.mid(sendOffset, first));
            sent = first;
        }
        if (sent < sendLength) {
            timer->start();
        } else {
            socket->flush();
            socket->disconnectFromHost();
        }
    }

    void sendError(QTcpSocket *socket, int code, const QByteArray &message)
    {
        QByteArray response;
        response.append("HTTP/1.1 " + QByteArray::number(code) + " " + message + "\r\n");
        response.append("Content-Length: 0\r\n");
        response.append("Connection: close\r\n");
        response.append("\r\n");
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }

private:
    QTcpServer *m_server;
    quint16 m_port = 0;
    QString m_filePath;
    QByteArray m_contentType;
    bool m_acceptRanges = true;
    int m_throttleChunkBytes = 0;
    int m_throttleIntervalMs = 0;
    QList<QTcpSocket *> m_pendingClients;
};
