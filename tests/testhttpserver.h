/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Shared mock HTTP server for dragon-sdl tests.
 * Supports Range requests so KIO seek via resume metadata works correctly.
 */

#pragma once

#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

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
            response.append("Accept-Ranges: bytes\r\n");
            response.append("Connection: close\r\n");
            response.append("\r\n");
        }

        socket->write(response);
        socket->write(content.mid(sendOffset, sendLength));
        socket->flush();
        socket->disconnectFromHost();
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
    QList<QTcpSocket *> m_pendingClients;
};
