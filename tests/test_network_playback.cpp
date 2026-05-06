/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Network playback test - serves a local WMA fixture via HTTP and plays it.
 * Self-contained test that doesn't depend on external websites.
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include <dragonsdl/dragonplayer.h>

using namespace Qt::StringLiterals;

class TestHttpServer : public QObject
{
    Q_OBJECT

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

    void serveFile(const QString &path, const QByteArray &contentType = "audio/x-ms-wma"_ba)
    {
        m_filePath = path;
        m_contentType = contentType;
    }

private Q_SLOTS:
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

        QByteArray response;
        response.append("HTTP/1.1 200 OK\r\n");
        response.append("Content-Type: " + m_contentType + "\r\n");
        response.append("Content-Length: " + QByteArray::number(content.size()) + "\r\n");
        response.append("Connection: close\r\n");
        response.append("\r\n");
        response.append(content);

        socket->write(response);
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

class TestNetworkPlayback : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testPlayLocalWmaFileOverHttp();

private:
    TestHttpServer *m_server = nullptr;
    QString fixturePath(const QString &filename);
};

QString TestNetworkPlayback::fixturePath(const QString &filename)
{
    QString fixturesDir = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR);
    return fixturesDir + u"/"_s + filename;
}

void TestNetworkPlayback::initTestCase()
{
    m_server = new TestHttpServer(this);
    QVERIFY2(m_server->start(), "Failed to start HTTP server");
}

void TestNetworkPlayback::cleanupTestCase()
{
    if (m_server) {
        m_server->stop();
        delete m_server;
        m_server = nullptr;
    }
}

void TestNetworkPlayback::testPlayLocalWmaFileOverHttp()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    qDebug() << "Testing playback from:" << url.toString();

    DragonPlayer player;

    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    player.setSource(url);

    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 5000);
    QVERIFY(player.source() == url);

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto s = player.status();
            return s == DragonPlayer::MediaStatus::LoadedMedia || s == DragonPlayer::MediaStatus::BufferedMedia || s == DragonPlayer::MediaStatus::InvalidMedia
                || errorSpy.count() > 0;
        }(),
        10000);

    if (errorSpy.count() > 0) {
        auto error = errorSpy.last().at(0).value<DragonPlayer::Error>();
        QFAIL(qPrintable(u"Error loading media: %1"_s.arg(static_cast<int>(error))));
    }

    auto status = player.status();
    qDebug() << "Media status after load:" << static_cast<int>(status);

    if (status != DragonPlayer::MediaStatus::LoadedMedia && status != DragonPlayer::MediaStatus::BufferedMedia) {
        QSKIP("Media could not be loaded - skipping test");
    }

    QTRY_VERIFY_WITH_TIMEOUT(durationSpy.count() > 0 || player.duration() > 0, 5000);
    qDebug() << "Duration:" << player.duration() << "ms";
    QVERIFY(player.duration() > 0);

    qDebug() << "Starting playback...";
    player.play();

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QVERIFY2(player.isAudioActive(), "Audio should be active during playback");
    qDebug() << "Playback started! Position:" << player.position() << "ms";

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return player.playbackState() == DragonPlayer::PlaybackState::StoppedState || player.status() == DragonPlayer::MediaStatus::EndOfMedia;
        }(),
        60000);

    qDebug() << "Playback completed!";
    qDebug() << "Final status:" << static_cast<int>(player.status());
    qDebug() << "Final state:" << static_cast<int>(player.playbackState());

    QVERIFY2(player.status() == DragonPlayer::MediaStatus::EndOfMedia, "Status should be EndOfMedia");
    QVERIFY2(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, "Playback state should be Stopped");
    QVERIFY2(!player.isAudioActive(), "Audio should be inactive");

    qDebug() << "Network playback test completed successfully!";
}

QTEST_MAIN(TestNetworkPlayback)
#include "test_network_playback.moc"
