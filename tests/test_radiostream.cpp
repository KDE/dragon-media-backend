/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "stream/dragonradiostream.h"

#include "test_utils.h"
#include "testhttpserver.h"

#include <QHash>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

using FieldHash = QHash<QString, QString>;

class TestRadioStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testConstruction();
    void testSetUrl();
    void testStartStop();

    void testReadBlocksUntilStop();
    void testStopAbortsStreamWithoutRead();
    void testReadCancellation();

    void testErrorSignal();
    void testMetadataParsing_data();
    void testMetadataParsing();

    void testStopWithoutStartIsNoOp();
    void testReadBehaviorWithStopToken();
    void testUrlChangeBehavior();
    void testIsAbortedFlag();
    void testMultipleStartStopCycles();
    void testSeekingCapabilities();
    void testSeekableChangedSignal();
    void testSeekableNeverAdvertised();
    void testSeekRejectedWithoutAcceptRanges();

    void testBufferingSignals();

    void testBackpressureCapsBuffer();
    void testBackpressureDrainsHeldTailOnEof();
    void testBackpressureNoDataLoss();

private:
    TestHttpServer *m_server = nullptr;

    QByteArray createHttpResponse(const QByteArray &body, bool includeIcyHeaders = false);
};

void TestRadioStream::initTestCase()
{
    QVERIFY(QNetworkAccessManager().supportedSchemes().contains("http"_L1));

    m_server = new TestHttpServer(this);
    QVERIFY(m_server->start());
}

void TestRadioStream::cleanupTestCase()
{
    if (m_server) {
        m_server->stop();
        delete m_server;
        m_server = nullptr;
    }
}

void TestRadioStream::testConstruction()
{
    DragonRadioStream stream;
    QVERIFY(!stream.isAborted());
}

void TestRadioStream::testSetUrl()
{
    DragonRadioStream stream;

    QUrl testUrl("http://example.com/stream"_L1);
    stream.setUrl(testUrl);
    QVERIFY(!stream.isAborted());

    stream.setUrl(QUrl{});
    QVERIFY(!stream.isAborted());
}

void TestRadioStream::testStartStop()
{
    DragonRadioStream stream;

    stream.setUrl(QUrl("http://example.com/test"_L1));

    stream.start();

    stream.stop();
    QVERIFY(stream.isAborted());

    stream.stop();
    QVERIFY(stream.isAborted());
}

void TestRadioStream::testReadBlocksUntilStop()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::atomic<bool> readCompleted{false};
    std::atomic<int> readResult{-2};
    std::vector<uint8_t> readBuffer(1024);

    std::thread readThread([&]() {
        std::stop_source stopSource;
        std::stop_token st = stopSource.get_token();

        readResult = stream.read(readBuffer, st);
        readCompleted = true;
    });

    QTest::qWait(100);

    QVERIFY(!readCompleted.load());

    stream.stop();

    readThread.join();

    QVERIFY(readCompleted.load());
    QCOMPARE(readResult.load(), -1);
}

void TestRadioStream::testStopAbortsStreamWithoutRead()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    stream.start();
    QTest::qWait(50);

    stream.stop();

    QVERIFY(stream.isAborted());
}

void TestRadioStream::testReadCancellation()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::atomic<bool> readStarted{false};
    std::atomic<int> readResult{-2};
    std::vector<uint8_t> buffer(1024);

    std::thread readThread([&]() {
        readStarted = true;
        std::stop_source cancelSource;
        std::stop_token st = cancelSource.get_token();

        readResult = stream.read(buffer, st);
    });

    QTRY_VERIFY(readStarted.load());

    QTest::qWait(50);

    stream.stop();

    readThread.join();

    QVERIFY(readResult.load() == -1);
}

void TestRadioStream::testErrorSignal()
{
    DragonRadioStream stream;

    QSignalSpy errorSpy(&stream, &DragonRadioStream::errorOccurred);

    stream.setUrl(QUrl("http://invalid-domain-that-does-not-exist-12345.com/stream"_L1));
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 10000);

    stream.stop();
}

void TestRadioStream::testMetadataParsing_data()
{
    QTest::addColumn<QByteArray>("metadata");
    QTest::addColumn<FieldHash>("expectedFields");

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "The Beatles - Hey Jude"_L1);
        QTest::newRow("single-field") << QByteArray("StreamTitle='The Beatles - Hey Jude';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "Some Song"_L1);
        QTest::newRow("title-only") << QByteArray("StreamTitle='Some Song';") << expected;
    }

    {
        FieldHash expected;
        QTest::newRow("empty-value") << QByteArray("StreamTitle='';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "Artist - Song"_L1);
        expected.insert("StreamUrl"_L1, "http://example.com"_L1);
        QTest::newRow("multiple-fields") << QByteArray("StreamTitle='Artist - Song';StreamUrl='http://example.com';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "Rock & Roll - Don't Stop"_L1);
        QTest::newRow("escaped-quote") << QByteArray("StreamTitle='Rock & Roll - Don''t Stop';") << expected;
    }
}

void TestRadioStream::testMetadataParsing()
{
    QFETCH(QByteArray, metadata);
    QFETCH(FieldHash, expectedFields);

    qRegisterMetaType<DragonIcyMetadata>("DragonIcyMetadata");

    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    QSignalSpy metadataSpy(&stream, &DragonRadioStream::metadataReady);
    QVERIFY(metadataSpy.isValid());

    stream.processMetadata(metadata);

    if (expectedFields.isEmpty()) {
        QCOMPARE(metadataSpy.count(), 0);
    } else {
        QCOMPARE(metadataSpy.count(), 1);

        const auto emitted = qvariant_cast<DragonIcyMetadata>(metadataSpy.takeFirst().at(0));

        if (expectedFields.contains("StreamTitle"_L1)) {
            QVERIFY2(emitted.hasStreamTitle(), "Expected StreamTitle to be set");
            QCOMPARE(emitted.streamTitle(), expectedFields.value("StreamTitle"_L1));
        }

        if (expectedFields.contains("StreamUrl"_L1)) {
            QVERIFY2(emitted.hasStreamUrl(), "Expected StreamUrl to be set");
            QCOMPARE(emitted.streamUrl(), expectedFields.value("StreamUrl"_L1));
        }

        const auto custom = emitted.customFields();
        for (auto it = expectedFields.constBegin(); it != expectedFields.constEnd(); ++it) {
            if (it.key() == "StreamTitle"_L1 || it.key() == "StreamUrl"_L1)
                continue;
            QVERIFY2(custom.contains(it.key()), qPrintable(u"Expected custom field '%1' to be present"_s.arg(it.key())));
            QCOMPARE(custom.value(it.key()), it.value());
        }
    }
}

void TestRadioStream::testStopWithoutStartIsNoOp()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    stream.stop();
    QVERIFY(stream.isAborted());

    stream.start();
    QTest::qWait(50);
    QVERIFY(!stream.isAborted());

    stream.stop();
    QVERIFY(stream.isAborted());
}

void TestRadioStream::testReadBehaviorWithStopToken()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::vector<uint8_t> buffer(1024);
    std::atomic<bool> readCompleted{false};
    std::atomic<int> readResult{-2};

    std::stop_source source;
    std::stop_token token = source.get_token();

    std::thread readThread([&]() {
        readResult = stream.read(buffer, token);
        readCompleted = true;
    });

    QTest::qWait(100);

    source.request_stop();

    readThread.join();

    QVERIFY(readCompleted.load());
    QVERIFY2(readResult.load() <= 0, qPrintable(u"Read should return <=0 after stop token cancellation, got %1"_s.arg(readResult.load())));
}

void TestRadioStream::testUrlChangeBehavior()
{
    DragonRadioStream stream;

    QUrl url1("http://example.com/stream1"_L1);
    stream.setUrl(url1);

    QUrl url2("http://example.com/stream2"_L1);
    stream.setUrl(url2);

    stream.start();
    QTest::qWait(100);
    stream.stop();

    QVERIFY(stream.isAborted());
}

void TestRadioStream::testIsAbortedFlag()
{
    DragonRadioStream stream;

    QVERIFY(!stream.isAborted());

    stream.setUrl(QUrl("http://invalid-domain-12345.com"_L1));
    stream.start();

    QSignalSpy errorSpy(&stream, &DragonRadioStream::errorOccurred);
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 10000);

    stream.stop();
    QVERIFY(stream.isAborted());

    stream.stop();
    QVERIFY(stream.isAborted());
}

void TestRadioStream::testMultipleStartStopCycles()
{
    DragonRadioStream stream;

    for (int i = 0; i < 3; ++i) {
        stream.setUrl(QUrl("http://example.com"_L1));
        stream.start();
        QTest::qWait(50);
        stream.stop();
    }

    QVERIFY(stream.isAborted());
}

void TestRadioStream::testSeekingCapabilities()
{
    QString wmaPath = TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(QFile::exists(wmaPath));

    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonRadioStream stream;
    stream.setUrl(url);
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(stream.size() > 0, 5000);

    qint64 totalSize = stream.size();
    QVERIFY(totalSize > 0);
    QCOMPARE(stream.position(), 0);

    std::vector<uint8_t> buffer(4096);
    std::atomic<int> readBytes{-1};
    std::atomic<bool> readDone{false};
    std::stop_source ss;

    std::thread readThread([&]() {
        readBytes = stream.read(buffer, ss.get_token());
        readDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(readDone.load(), 5000);
    readThread.join();

    QVERIFY(readBytes.load() > 0);
    QCOMPARE(stream.position(), readBytes.load());

    qint64 seekTarget = totalSize / 2;
    qint64 seekResult = stream.seek(seekTarget);
    QCOMPARE(seekResult, seekTarget);
    QCOMPARE(stream.position(), seekTarget);

    readDone = false;
    readBytes = -1;

    std::thread readThread2([&]() {
        readBytes = stream.read(buffer, ss.get_token());
        readDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(readDone.load(), 5000);
    readThread2.join();

    QVERIFY(readBytes.load() > 0);
    QCOMPARE(stream.position(), seekTarget + readBytes.load());
}

void TestRadioStream::testSeekableChangedSignal()
{
    QString wmaPath = TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(QFile::exists(wmaPath));

    m_server->setAcceptRanges(true);
    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonRadioStream stream;
    stream.setUrl(url);

    QSignalSpy seekableSpy(&stream, &DragonStream::seekableChanged);
    QVERIFY(seekableSpy.isValid());

    QVERIFY(!stream.isSeekable());

    stream.start();
    QTRY_COMPARE_WITH_TIMEOUT(stream.isSeekable(), true, 5000);
    QVERIFY(seekableSpy.contains(QList<QVariant>{true}));

    stream.stop();
    stream.start();
    QTRY_COMPARE_WITH_TIMEOUT(stream.isSeekable(), true, 5000);
    QVERIFY(seekableSpy.contains(QList<QVariant>{false}));
}

void TestRadioStream::testSeekableNeverAdvertised()
{
    QString wmaPath = TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(QFile::exists(wmaPath));

    m_server->setAcceptRanges(false);
    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonRadioStream stream;
    stream.setUrl(url);

    QSignalSpy seekableSpy(&stream, &DragonStream::seekableChanged);
    QVERIFY(seekableSpy.isValid());

    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(stream.size() > 0, 5000);

    QVERIFY(!QTest::qWaitFor(
        [&]() {
            return stream.isSeekable() || seekableSpy.contains(QList<QVariant>{true});
        },
        500));

    QVERIFY(!stream.isSeekable());
    QVERIFY(!seekableSpy.contains(QList<QVariant>{true}));

    m_server->setAcceptRanges(true);
}

void TestRadioStream::testSeekRejectedWithoutAcceptRanges()
{
    QString wmaPath = TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(QFile::exists(wmaPath));

    m_server->setAcceptRanges(false);
    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonRadioStream stream;
    stream.setUrl(url);
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(stream.size() > 0, 5000);

    std::vector<uint8_t> buffer(4096);
    std::atomic<int> readBytes{-1};
    std::atomic<bool> readDone{false};
    std::stop_source ss;

    std::thread readThread([&]() {
        readBytes = stream.read(buffer, ss.get_token());
        readDone = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(readDone.load(), 5000);
    readThread.join();

    QVERIFY(readBytes.load() > 0);

    QCOMPARE(stream.seek(1024), -1);
    QCOMPARE(stream.position(), qint64(readBytes.load()));

    m_server->setAcceptRanges(true);
}

QByteArray TestRadioStream::createHttpResponse(const QByteArray &body, bool includeIcyHeaders)
{
    QByteArray response;
    response.append("HTTP/1.1 200 OK\r\n");
    response.append("Content-Type: audio/mpeg\r\n");

    if (includeIcyHeaders) {
        response.append("icy-metaint: 8192\r\n");
        response.append("icy-name: Test Radio\r\n");
    }

    response.append("Content-Length: ");
    response.append(QByteArray::number(body.size()));
    response.append("\r\n");
    response.append("\r\n");
    response.append(body);

    return response;
}

QTEST_MAIN(TestRadioStream)

void TestRadioStream::testBufferingSignals()
{
    QString wmaPath = TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(QFile::exists(wmaPath));

    QFile f(wmaPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray fileData = f.readAll();
    f.close();
    QVERIFY(!fileData.isEmpty());

    // Create a ~4MB pseudo-stream by repeating the fixture
    const qint64 targetSize = 4 * 1024 * 1024;
    QByteArray bigPayload;
    bigPayload.reserve(targetSize);
    while (bigPayload.size() < targetSize) {
        bigPayload.append(fileData);
    }
    bigPayload.resize(targetSize);

    QTcpServer server;
    QList<QTcpSocket *> clients;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            clients.append(sock);

            QByteArray headers;
            headers.append("HTTP/1.1 200 OK\r\n");
            headers.append("Content-Type: application/octet-stream\r\n");
            headers.append("Content-Length: " + QByteArray::number(bigPayload.size()) + "\r\n");
            headers.append("Connection: keep-alive\r\n");
            headers.append("\r\n");
            sock->write(headers);
            sock->flush();

            // Send steadily at a moderate rate the reader controls
            // buffering transitions by alternating drain and wait phases
            auto *timer = new QTimer(sock);
            auto offset = std::make_shared<int>(0);
            QObject::connect(timer, &QTimer::timeout, sock, [&, sock, timer, offset]() {
                if (*offset >= bigPayload.size()) {
                    sock->disconnectFromHost();
                    timer->stop();
                    return;
                }
                const int len = std::min<int>(8192, bigPayload.size() - *offset);
                sock->write(bigPayload.mid(*offset, len));
                sock->flush();
                *offset += len;
            });
            timer->start(1);
        }
    });

    QVERIFY(server.listen(QHostAddress::LocalHost));

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.serverPort());
    url.setPath(u"/stream"_s);

    DragonRadioStream stream;
    QSignalSpy bufferingSpy(&stream, &DragonRadioStream::streamBuffering);
    QSignalSpy bufferedSpy(&stream, &DragonRadioStream::streamBuffered);

    stream.setUrl(url);
    stream.start();

    // Give the server time to fill the initial buffer above HIGH_WATER_MARK
    QTest::qWait(200);

    std::stop_source ss;
    std::vector<uint8_t> buf(65536);
    std::atomic<int> totalRead{0};
    // Phase control: 0=drain, 1=pause (let buffer refill), 2=done
    std::atomic<int> phase{0};

    std::thread reader([&]() {
        while (!ss.stop_requested()) {
            const int p = phase.load(std::memory_order_acquire);
            if (p == 2)
                break;
            if (p == 1) {
                // Pause phase: don't read, let buffer accumulate
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // Drain phase: read everything available
            int n = stream.read(buf, ss.get_token());
            if (n <= 0)
                break;
            totalRead += n;
        }
    });

    auto cleanup = qScopeGuard([&]() {
        phase.store(2, std::memory_order_release);
        ss.request_stop();
        if (reader.joinable())
            reader.join();
        stream.stop();
        server.close();
        for (auto *c : clients)
            c->deleteLater();
    });

    // Let the drain phase run until buffer is empty and streamBuffering fires
    QTRY_VERIFY_WITH_TIMEOUT(bufferingSpy.count() > 0, 10000);

    // Switch to pause phase: reader stops reading, buffer accumulates
    phase.store(1, std::memory_order_release);

    // Wait for buffer to refill above HIGH_WATER_MARK and streamBuffered to fire
    QTRY_VERIFY_WITH_TIMEOUT(bufferedSpy.count() > 0, 10000);

    // Resume reading to consume more data
    phase.store(0, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(totalRead.load() > 262144, 10000);

    QVERIFY2(totalRead.load() > 0, "Should have read some data");
}

void TestRadioStream::testBackpressureCapsBuffer()
{
    // Send a large payload faster than the (paused) reader consumes.
    // The buffer should be capped at MAX_BUFFER_BYTES.
    const QByteArray payload = QByteArray("0123456789").repeated(524288); // ~5.2 MiB

    QTcpServer server;
    QList<QTcpSocket *> clients;

    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            clients.append(sock);

            QByteArray headers;
            headers.append("HTTP/1.1 200 OK\r\n");
            headers.append("Content-Type: application/octet-stream\r\n");
            headers.append("Content-Length: " + QByteArray::number(payload.size()) + "\r\n");
            headers.append("Connection: close\r\n");
            headers.append("\r\n");
            sock->write(headers);
            sock->flush();

            auto *timer = new QTimer(sock);
            auto offset = std::make_shared<int>(0);
            connect(timer, &QTimer::timeout, sock, [&, sock, timer, offset]() {
                if (*offset >= payload.size()) {
                    sock->disconnectFromHost();
                    timer->stop();
                    return;
                }
                const int len = static_cast<int>(std::min<qsizetype>(8192, payload.size() - *offset));
                sock->write(payload.mid(*offset, len));
                sock->flush();
                *offset += len;
            });
            timer->start(1);
        }
    });

    QVERIFY(server.listen(QHostAddress::LocalHost));

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.serverPort());
    url.setPath(u"/stream"_s);

    DragonRadioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let data arrive without reading -- buffer should be capped.
    QTest::qWait(3000);

    QVERIFY2(stream.m_bufferDepth.load() <= DragonRadioStream::MAX_BUFFER_BYTES + 8192,
             qPrintable(u"Buffer depth %1 exceeds cap + tolerance"_s.arg(stream.m_bufferDepth.load())));
    QVERIFY2(stream.m_bufferDepth.load() > 0, "Buffer should have some data after waiting");

    stream.stop();
    server.close();
    for (auto *c : clients)
        c->deleteLater();
}

void TestRadioStream::testBackpressureDrainsHeldTailOnEof()
{
    // Send a payload larger than the cap with a dribbling server. When the
    // reader is paused, the tail is held in the reply buffer by backpressure.
    // On EOF, the tail must be drained.
    const QByteArray payload = QByteArray("ABCDEFGHIJ").repeated(524288); // ~5.2 MiB

    QTcpServer server;
    QList<QTcpSocket *> clients;

    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            clients.append(sock);

            sock->readAll(); // consume request

            QByteArray headers;
            headers.append("HTTP/1.1 200 OK\r\n");
            headers.append("Content-Type: application/octet-stream\r\n");
            headers.append("Content-Length: " + QByteArray::number(payload.size()) + "\r\n");
            headers.append("Connection: close\r\n");
            headers.append("\r\n");
            sock->write(headers);
            sock->flush();

            auto *timer = new QTimer(sock);
            auto offset = std::make_shared<int>(0);
            connect(timer, &QTimer::timeout, sock, [&, sock, timer, offset]() {
                if (*offset >= payload.size()) {
                    sock->disconnectFromHost();
                    timer->stop();
                    return;
                }
                const int len = static_cast<int>(std::min<qsizetype>(8192, payload.size() - *offset));
                sock->write(payload.mid(*offset, len));
                sock->flush();
                *offset += len;
            });
            timer->start(1);
        }
    });

    QVERIFY(server.listen(QHostAddress::LocalHost));

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.serverPort());
    url.setPath(u"/stream"_s);

    DragonRadioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let buffer fill to cap.
    QTest::qWait(2000);
    QVERIFY2(stream.m_bufferDepth.load() > 0, "Buffer should have data");

    // Now read everything. The tail held by backpressure should be
    // drained on EOF.
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

    QVERIFY2(gotEof.load(), "Should have received EOF");
    QCOMPARE(received.size(), payload.size());
    QCOMPARE(received, payload);

    stream.stop();
    server.close();
    for (auto *c : clients)
        c->deleteLater();
}

void TestRadioStream::testBackpressureNoDataLoss()
{
    // Send data with a dribbling server, pause the reader to trigger
    // backpressure, then resume and verify all data is received.
    const QByteArray payload = QByteArray("0123456789").repeated(524288); // ~5.2 MiB

    QTcpServer server;
    QList<QTcpSocket *> clients;

    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            clients.append(sock);

            sock->readAll(); // consume request

            QByteArray headers;
            headers.append("HTTP/1.1 200 OK\r\n");
            headers.append("Content-Type: application/octet-stream\r\n");
            headers.append("Content-Length: " + QByteArray::number(payload.size()) + "\r\n");
            headers.append("Connection: close\r\n");
            headers.append("\r\n");
            sock->write(headers);
            sock->flush();

            auto *timer = new QTimer(sock);
            auto offset = std::make_shared<int>(0);
            connect(timer, &QTimer::timeout, sock, [&, sock, timer, offset]() {
                if (*offset >= payload.size()) {
                    sock->disconnectFromHost();
                    timer->stop();
                    return;
                }
                const int len = static_cast<int>(std::min<qsizetype>(4096, payload.size() - *offset));
                sock->write(payload.mid(*offset, len));
                sock->flush();
                *offset += len;
            });
            timer->start(1);
        }
    });

    QVERIFY(server.listen(QHostAddress::LocalHost));

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(server.serverPort());
    url.setPath(u"/stream"_s);

    DragonRadioStream stream;
    stream.setUrl(url);
    stream.start();

    // Let buffer fill and cap.
    QTest::qWait(2000);

    // Read everything with a brief pause in the middle to exercise
    // the suspend/resume cycle.
    std::stop_source ss;
    std::vector<uint8_t> buf(65536);
    std::atomic<int> totalRead{0};
    std::atomic<bool> gotEof{false};
    QByteArray received;
    received.reserve(payload.size());

    std::thread reader([&]() {
        bool pausedOnce = false;
        while (!ss.stop_requested()) {
            if (totalRead.load() > 1024 * 1024 && !pausedOnce) {
                pausedOnce = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
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

    QTRY_VERIFY_WITH_TIMEOUT(gotEof.load() || totalRead.load() >= payload.size(), 30000);
    reader.join();

    QCOMPARE(received.size(), payload.size());
    QCOMPARE(received, payload);

    stream.stop();
    server.close();
    for (auto *c : clients)
        c->deleteLater();
}

#include "test_radiostream.moc"
