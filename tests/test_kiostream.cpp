/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "testhttpserver.h"

#include "stream/dragonkiostream.h"

#include <QScopeGuard>
#include <QTimer>
#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

class TestKioStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testConstruction();
    void testSetUrl();
    void testStartStop();

    void testReadLocalFile();
    void testReadNonExistentFile();
    void testReadBlocksUntilStop();
    void testReadCancellation();
    void testSeeking();
    void testSftpAssumedSeekable();
    void testNonSftpNotSeekable();

    void testHttpKioRead();
    void testHttpKioSeekDataCorrect();

    void testHttpKioSeekDoesNotCorruptStream();
    void testHttpKioRapidSeek();
    void testHttpKioSeekWhileReading();
    void testHttpKioSeekWhileReaderBlockedNoSpuriousEof();

    void testBackpressureCapsBuffer();
    void testBackpressureResumesAfterDrain();
    void testBackpressureNoDataLoss();

private:
    QString m_testFilePath;
    QString m_bigFilePath;
    TestHttpServer *m_httpServer = nullptr;
};

void TestKioStream::initTestCase()
{
    QTemporaryFile tempFile;
    tempFile.setAutoRemove(false);
    if (tempFile.open()) {
        tempFile.write("Hello KIO world!");
        m_testFilePath = tempFile.fileName();
        tempFile.close();
    }

    QTemporaryFile bigFile;
    bigFile.setAutoRemove(false);
    if (bigFile.open()) {
        bigFile.write(QByteArray("0123456789").repeated(6400));
        m_bigFilePath = bigFile.fileName();
        bigFile.close();
    }

    m_httpServer = new TestHttpServer(this);
    QVERIFY2(m_httpServer->start(), "Failed to start HTTP server");
}

void TestKioStream::cleanupTestCase()
{
    if (!m_testFilePath.isEmpty()) {
        QFile::remove(m_testFilePath);
    }
    if (!m_bigFilePath.isEmpty()) {
        QFile::remove(m_bigFilePath);
    }
    if (m_httpServer) {
        m_httpServer->stop();
        delete m_httpServer;
        m_httpServer = nullptr;
    }
}

void TestKioStream::testConstruction()
{
    DragonKioStream stream;
    QCOMPARE(stream.size(), -1);
    QCOMPARE(stream.position(), 0);
    QCOMPARE(stream.isSeekable(), false);
}

void TestKioStream::testSetUrl()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    QCOMPARE(stream.size(), -1);
}

void TestKioStream::testStartStop()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    stream.start();
    stream.stop();
    QCOMPARE(stream.position(), 0);
}

void TestKioStream::testReadLocalFile()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
        QCOMPARE(bytesRead.load(), 16);
        QCOMPARE(QString::fromUtf8(reinterpret_cast<const char *>(buffer.data()), bytesRead.load()), QStringLiteral("Hello KIO world!"));
    }

    bytesRead = -2;
    {
        std::jthread readThread2([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY(bytesRead.load() != -2);
        QCOMPARE(bytesRead.load(), 0);
    }
}

void TestKioStream::testReadNonExistentFile()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(QStringLiteral("/non/existent/file/12345")));

    QSignalSpy errorSpy(&stream, &DragonKioStream::errorOccurred);
    stream.start();

    QTRY_VERIFY(errorSpy.count() > 0);

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    int result = stream.read(buffer, st);
    QCOMPARE(result, -1);
}

void TestKioStream::testReadBlocksUntilStop()
{
    DragonKioStream stream;
    std::atomic<bool> readCompleted{false};
    std::atomic<int> readResult{-2};
    std::vector<uint8_t> buffer(1024);

    {
        std::jthread readThread([&]() {
            std::stop_source stopSource;
            readResult = stream.read(buffer, stopSource.get_token());
            readCompleted = true;
        });

        QTest::qWait(100);
        QVERIFY(!readCompleted.load());

        stream.stop();
    }
    QVERIFY(readCompleted.load());
    QCOMPARE(readResult.load(), -1);
}

void TestKioStream::testReadCancellation()
{
    DragonKioStream stream;
    std::vector<uint8_t> buffer(1024);

    std::stop_source stopSource;
    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, stopSource.get_token());
        });

        QTest::qWait(100);
        QVERIFY(bytesRead.load() == -2);

        stopSource.request_stop();
    }
    QCOMPARE(bytesRead.load(), -1);
}

void TestKioStream::testSeeking()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QCOMPARE(stream.size(), 16);

    stream.seek(6);
    QCOMPARE(stream.position(), 6);

    bytesRead = -2;
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QCOMPARE(bytesRead.load(), 10);
    QCOMPARE(stream.position(), 16);
    QCOMPARE(QString::fromUtf8(reinterpret_cast<const char *>(buffer.data()), bytesRead.load()), QStringLiteral("KIO world!"));
}

static QUrl httpUrl(TestHttpServer *server, const QString &path)
{
    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server->port());
    url.setPath(path);
    return url;
}

void TestKioStream::testSftpAssumedSeekable()
{
    DragonKioStream stream;
    stream.setUrl(QUrl(u"sftp://user@example.org/music/track.flac"_s));

    QSignalSpy seekableSpy(&stream, &DragonStream::seekableChanged);
    QVERIFY(seekableSpy.isValid());

    stream.start();
    QCOMPARE(stream.isSeekable(), true);
    QVERIFY(seekableSpy.contains(QList<QVariant>{true}));

    stream.stop();
}

void TestKioStream::testNonSftpNotSeekable()
{
    DragonKioStream localStream;
    QSignalSpy localSpy(&localStream, &DragonStream::seekableChanged);
    QVERIFY(localSpy.isValid());
    localStream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    QCOMPARE(localStream.isSeekable(), false);
    localStream.start();
    QVERIFY(!localSpy.contains(QList<QVariant>{true}));
    localStream.stop();

    DragonKioStream httpStream;
    httpStream.setUrl(QUrl(u"http://example.org/big.dat"_s));
    QCOMPARE(httpStream.isSeekable(), false);
}

void TestKioStream::testHttpKioRead()
{
    m_httpServer->serveFile(m_bigFilePath);

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QCOMPARE(bytesRead.load(), 1024);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer.data()), 10), QByteArray("0123456789"));
}

void TestKioStream::testHttpKioSeekDataCorrect()
{
    m_httpServer->serveFile(m_bigFilePath);

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }
    QVERIFY(bytesRead.load() > 0);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer.data()), 10), QByteArray("0123456789"));

    stream.seek(2048);
    QCOMPARE(stream.position(), 2048);

    bytesRead = -2;
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QVERIFY2(bytesRead.load() > 0, "Read after seek should return data");

    QByteArray expected = QByteArray("0123456789").repeated(6400).mid(2048, bytesRead.load());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer.data()), bytesRead.load()), expected);
}

void TestKioStream::testHttpKioSeekDoesNotCorruptStream()
{
    m_httpServer->serveFile(m_bigFilePath);

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }
    QVERIFY(bytesRead.load() > 0);

    stream.seek(32768);
    QCOMPARE(stream.position(), 32768);

    QCoreApplication::processEvents();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    bytesRead = -2;
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QVERIFY2(bytesRead.load() > 0, qPrintable(u"Read after seek returned %1 stale signal from killed job corrupted stream state"_s.arg(bytesRead.load())));

    QByteArray expected = QByteArray("0123456789").repeated(6400).mid(32768, bytesRead.load());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer.data()), bytesRead.load()), expected);
}

void TestKioStream::testHttpKioRapidSeek()
{
    m_httpServer->serveFile(m_bigFilePath);

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> bytesRead{-2};
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }
    QVERIFY(bytesRead.load() > 0);

    for (int i = 0; i < 5; ++i) {
        stream.seek(i * 1024);
    }

    QCoreApplication::processEvents();
    QTest::qWait(100);
    QCoreApplication::processEvents();

    bytesRead = -2;
    {
        std::jthread readThread([&]() {
            bytesRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(bytesRead.load() != -2, 5000);
    }

    QVERIFY2(bytesRead.load() > 0, qPrintable(u"Stream broken after rapid seeks returned %1"_s.arg(bytesRead.load())));

    const int finalOffset = 4 * 1024;
    for (int i = 0; i < bytesRead.load(); ++i) {
        char expected = '0' + ((finalOffset + i) % 10);
        QVERIFY2(static_cast<char>(buffer[static_cast<size_t>(i)]) == expected,
                 qPrintable(u"Content mismatch at offset %1 (byte %2): expected '%3', got '%4'"_s.arg(finalOffset + i)
                                .arg(i)
                                .arg(expected)
                                .arg(static_cast<char>(buffer[static_cast<size_t>(i)]))));
    }
}

void TestKioStream::testHttpKioSeekWhileReading()
{
    m_httpServer->serveFile(m_bigFilePath);
    m_httpServer->setThrottle(512, 10);
    const auto throttleGuard = qScopeGuard([this]() {
        m_httpServer->clearThrottle();
    });

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(4096);
    std::atomic<int> dataReads{0};
    std::atomic<int> spuriousEof{0};
    std::atomic<int> contentMismatches{0};
    std::atomic<qint64> firstMismatchAt{-1};
    std::atomic<int> firstMismatchExpected{-1};
    std::atomic<int> firstMismatchGot{-1};
    std::atomic<qint64> verifiedPostSeekBytes{0};
    std::atomic<bool> seekDone{false};

    std::jthread reader([&](std::stop_token st) {
        while (!st.stop_requested()) {
            const bool verify = seekDone.load();
            const qint64 before = verify ? stream.position() : 0;
            const int n = stream.read(buffer, st);
            if (n == 0) {
                spuriousEof.fetch_add(1);
                break;
            }
            if (n < 0) {
                continue;
            }
            dataReads.fetch_add(1);
            if (!verify) {
                continue;
            }
            for (int i = 0; i < n; ++i) {
                const int expected = '0' + static_cast<int>((before + i) % 10);
                const int got = static_cast<int>(buffer[static_cast<size_t>(i)]);
                if (got != expected && contentMismatches.fetch_add(1) == 0) {
                    firstMismatchAt.store(before + i);
                    firstMismatchExpected.store(expected);
                    firstMismatchGot.store(got);
                }
            }
            verifiedPostSeekBytes.fetch_add(n);
        }
    });

    QTRY_VERIFY_WITH_TIMEOUT(dataReads.load() > 0, 10000);

    stream.seek(1024);
    QCOMPARE(stream.position(), 1024);
    seekDone.store(true);

    QTRY_VERIFY2_WITH_TIMEOUT(verifiedPostSeekBytes.load() >= 1024,
                              qPrintable(u"Stream unusable after seek-while-reading: only %1 verified bytes"_s.arg(verifiedPostSeekBytes.load())),
                              10000);

    stream.stop();

    QVERIFY2(dataReads.load() > 0, "Reader never received data");
    QVERIFY2(spuriousEof.load() == 0, qPrintable(u"Reader saw a spurious EOF (%1 time(s)) while data remained"_s.arg(spuriousEof.load())));
    const QString mismatchMessage = contentMismatches.load() > 0
        ? u"%1 content mismatch(es) after seek, first at offset %2: expected '%3', got '%4'"_s.arg(contentMismatches.load())
              .arg(firstMismatchAt.load())
              .arg(QChar(firstMismatchExpected.load()))
              .arg(QChar(firstMismatchGot.load()))
        : QString();
    QVERIFY2(contentMismatches.load() == 0, qPrintable(mismatchMessage));
}

/*
 * Throttled HTTP server that sends data in small chunks at a steady rate.
 * Used for backpressure tests where the consumer must be slower than the
 * network to exercise the buffer cap.
 */
class ThrottledHttpServer : public QObject
{
    Q_OBJECT
public:
    explicit ThrottledHttpServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(new QTcpServer(this))
    {
        connect(m_server, &QTcpServer::newConnection, this, &ThrottledHttpServer::onNewConnection);
    }

    bool start(quint16 port = 0)
    {
        return m_server->listen(QHostAddress::LocalHost, port);
    }

    [[nodiscard]] quint16 port() const
    {
        return m_server->serverPort();
    }

    void setPayload(const QByteArray &data, int chunkSize = 8192, int intervalMs = 1)
    {
        m_payload = data;
        m_chunkSize = chunkSize;
        m_intervalMs = intervalMs;
    }

    void stop()
    {
        m_server->close();
        for (auto *sock : m_clients) {
            sock->disconnectFromHost();
            sock->deleteLater();
        }
        m_clients.clear();
    }

private:
    void onNewConnection()
    {
        while (m_server->hasPendingConnections()) {
            auto *sock = m_server->nextPendingConnection();
            m_clients.append(sock);
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
                // Consume the HTTP request line
                sock->readAll();
                sendHeaders(sock);
                startDribbling(sock);
            });
            connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
                m_clients.removeAll(sock);
                sock->deleteLater();
            });
        }
    }

    void sendHeaders(QTcpSocket *sock)
    {
        QByteArray headers;
        headers.append("HTTP/1.1 200 OK\r\n");
        headers.append("Content-Type: application/octet-stream\r\n");
        headers.append("Content-Length: " + QByteArray::number(m_payload.size()) + "\r\n");
        headers.append("Accept-Ranges: bytes\r\n");
        headers.append("Connection: close\r\n");
        headers.append("\r\n");
        sock->write(headers);
        sock->flush();
    }

    void startDribbling(QTcpSocket *sock)
    {
        auto *timer = new QTimer(sock);
        auto offset = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, sock, [this, sock, timer, offset]() {
            if (*offset >= m_payload.size()) {
                sock->disconnectFromHost();
                timer->stop();
                return;
            }
            const int len = static_cast<int>(std::min<qsizetype>(m_chunkSize, m_payload.size() - *offset));
            sock->write(m_payload.mid(*offset, len));
            sock->flush();
            *offset += len;
        });
        timer->start(m_intervalMs);
    }

    QTcpServer *m_server;
    QByteArray m_payload;
    int m_chunkSize = 8192;
    int m_intervalMs = 1;
    QList<QTcpSocket *> m_clients;
};

void TestKioStream::testBackpressureCapsBuffer()
{
    // Send 8 MiB of data (2x the 4 MiB cap) with the reader paused.
    // The buffer should never significantly exceed MAX_BUFFER_BYTES.
    const QByteArray payload = QByteArray("0123456789").repeated(838860); // ~8.4 MiB

    ThrottledHttpServer server;
    server.setPayload(payload, 4096, 0);
    QVERIFY(server.start());

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.port());
    url.setPath(u"/big.dat"_s);

    DragonKioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let data accumulate without reading buffer should be capped.
    QTest::qWait(3000);

    QVERIFY2(stream.m_bufferDepth.load() <= DragonKioStream::MAX_BUFFER_BYTES + 8192,
             qPrintable(u"Buffer depth %1 exceeds cap + tolerance"_s.arg(stream.m_bufferDepth.load())));
    QVERIFY2(stream.m_bufferDepth.load() > 0, "Buffer should have some data after waiting");

    stream.stop();
    server.stop();
}

void TestKioStream::testBackpressureResumesAfterDrain()
{
    // Send a payload larger than the cap. After the buffer fills and the job
    // suspends, drain the entire buffer and verify the stream reaches EOF.
    // This proves the job is resumed after draining otherwise the stream
    // would stall at the cap forever.
    const QByteArray payload = QByteArray("0123456789").repeated(838860); // ~8.4 MiB

    ThrottledHttpServer server;
    server.setPayload(payload, 4096, 0);
    QVERIFY(server.start());

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.port());
    url.setPath(u"/big.dat"_s);

    DragonKioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let buffer fill to cap and suspend the job.
    QTest::qWait(3000);
    QVERIFY2(stream.m_bufferDepth.load() > 0, "Buffer should have data");
    QVERIFY2(stream.m_suspended, "Job should be suspended after hitting the cap");

    // Now read everything the job must be resumed for the stream to complete.
    std::stop_source ss;
    std::vector<uint8_t> buf(65536);
    std::atomic<int> totalRead{0};
    std::atomic<bool> gotEof{false};
    QByteArray received;
    received.reserve(payload.size());

    std::thread reader([&]() {
        while (!ss.stop_requested()) {
            int n = stream.read(buf, ss.get_token());
            if (n < 0)
                break;
            if (n == 0) {
                gotEof = true;
                break;
            }
            received.append(reinterpret_cast<const char *>(buf.data()), n);
            totalRead += n;
        }
    });

    QTRY_VERIFY_WITH_TIMEOUT(gotEof.load(), 30000);
    reader.join();

    QVERIFY2(gotEof.load(), "Stream should have reached EOF job was not resumed after drain");
    QCOMPARE(received.size(), payload.size());
    QCOMPARE(received, payload);

    stream.stop();
    server.stop();
}

void TestKioStream::testBackpressureNoDataLoss()
{
    // Send exactly 5 MiB (> 4 MiB cap) and verify all bytes are received.
    const QByteArray payload = QByteArray("ABCDEFGHIJKLMNOPQRSTUVWXYZ").repeated(201326); // ~5.2 MiB

    ThrottledHttpServer server;
    server.setPayload(payload, 4096, 0);
    QVERIFY(server.start());

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.port());
    url.setPath(u"/big.dat"_s);

    DragonKioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let the buffer fill and suspend.
    QTest::qWait(2000);

    // Now read everything and verify integrity.
    std::stop_source ss;
    std::vector<uint8_t> buf(65536);
    std::atomic<int> totalRead{0};
    std::atomic<bool> gotEof{false};
    QByteArray received;
    received.reserve(payload.size());

    std::thread reader([&]() {
        while (!ss.stop_requested()) {
            int n = stream.read(buf, ss.get_token());
            if (n < 0)
                break;
            if (n == 0) {
                gotEof = true;
                break;
            }
            received.append(reinterpret_cast<const char *>(buf.data()), n);
            totalRead += n;
        }
    });

    QTRY_VERIFY_WITH_TIMEOUT(gotEof.load(), 30000);
    reader.join();

    QCOMPARE(received.size(), payload.size());
    QCOMPARE(received, payload);

    stream.stop();
    server.stop();
}

void TestKioStream::testHttpKioSeekWhileReaderBlockedNoSpuriousEof()
{
    ThrottledHttpServer server;
    server.setPayload(QByteArray("0123456789").repeated(6400), 512, 10);
    QVERIFY(server.start());

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.port());
    url.setPath(u"/big.dat"_s);

    DragonKioStream stream;
    stream.setUrl(url);
    stream.start();

    std::vector<uint8_t> buffer(4096);
    std::atomic<int> dataReads{0};
    std::atomic<int> spuriousEof{0};

    std::jthread reader([&](std::stop_token st) {
        while (!st.stop_requested()) {
            const int n = stream.read(buffer, st);
            if (n == 0) {
                spuriousEof++;
                break;
            }
            if (n > 0) {
                dataReads++;
            }
            // n < 0: aborted by a concurrent seek()/stop(); loop and read
            // again from the new position.
        }
    });

    {
        for (int i = 0; i < 20; ++i) {
            const int readsBeforeSeek = dataReads.load();
            QTRY_VERIFY2_WITH_TIMEOUT(dataReads.load() > readsBeforeSeek,
                                      qPrintable(u"Seek cycle %1: reader received no data before the next seek"_s.arg(i)),
                                      10000);
            stream.seek((i + 1) * 1024);
            QCoreApplication::processEvents();
        }

        const int readsBeforeStop = dataReads.load();
        QTRY_VERIFY2_WITH_TIMEOUT(dataReads.load() > readsBeforeStop, "Final seek delivered no data to the reader", 10000);
    }

    stream.stop();
    server.stop();

    QVERIFY2(dataReads.load() > 0, "Reader never received data; seek race window not exercised");
    QVERIFY2(spuriousEof.load() == 0, qPrintable(u"Reader saw a spurious EOF (%1 time(s)) while data remained"_s.arg(spuriousEof.load())));
}

QTEST_MAIN(TestKioStream)
#include "test_kiostream.moc"
