/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "stream/http/httpbodyframer.h"
#include "stream/http/httpprotocol.h"
#include "stream/http/httpsocket.h"
#include "stream/http/icydemuxer.h"
#include "stream/http/radiostream.h"

#include "streamtestserver.h"
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

class ScopedProxyOverride
{
public:
    explicit ScopedProxyOverride(const QNetworkProxy &proxy)
        : m_wasSystem(QNetworkProxyFactory::usesSystemConfiguration())
        , m_previous(m_wasSystem ? QNetworkProxy(QNetworkProxy::NoProxy) : QNetworkProxy::applicationProxy())
    {
        QNetworkProxy::setApplicationProxy(proxy);
    }

    explicit ScopedProxyOverride(QNetworkProxyFactory *factory)
        : m_wasSystem(QNetworkProxyFactory::usesSystemConfiguration())
        , m_previous(m_wasSystem ? QNetworkProxy(QNetworkProxy::NoProxy) : QNetworkProxy::applicationProxy())
    {
        QNetworkProxyFactory::setApplicationProxyFactory(factory);
    }

    ~ScopedProxyOverride()
    {
        if (m_wasSystem) {
            QNetworkProxyFactory::setUseSystemConfiguration(true);
        } else {
            QNetworkProxy::setApplicationProxy(m_previous);
        }
    }

    Q_DISABLE_COPY_MOVE(ScopedProxyOverride)

private:
    bool m_wasSystem;
    QNetworkProxy m_previous;
};

class TestRadioStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testLifecycle();
    void testReadBlocksUntilStop();
    void testErrorSignal();
    void testMetadataParsing_data();
    void testMetadataParsing();

    void testReadBehaviorWithStopToken();
    void testIsAbortedFlag();
    void testSeekingCapabilities();
    void testSeekableChangedSignal();
    void testSeekRejectedWhenRangeIsIgnored();

    void testBufferingSignals();
    void testBackpressure();

    void testIcyResponseParsing();
    void testBodyFramerDirect();
    void testStopCancelsPendingReconnectTimer();
    void testInformational1xxWithPipelinedResponse();
    void testEndStreamReentrancy();
    void testIcyDemuxerReentrancy();
    void testIcyDemuxerEmptyTitleClearing();
    void testIcyDemuxerSplitMetadata();
    void testHttpRequestSetHeaderReplaces();
    void testStreamStalledStopCancelsReconnect();
    void testContentRangeParsing();
    void testHostHeaderAndRequestTarget();
    void testSocketErrorAndRetryMapping();
    void testSeekToZeroWith200Ok();
    void testTlsSessionTicketResumption();
    void testParseResponseHeadersDirect();
    void testHttpSocketRedirectErrors();
    void testHttpSocketHeaderEdgeCases();
    void testHttpSocketTruncatedBody();
    void testRangeReconnectionOnDrop();

    void testDialectsPlayAudio_data();
    void testDialectsPlayAudio();
    void testDialectsReportSize();
    void testDialectsEndOnServerClose();
    void testDialectsAdvertiseRanges_data();
    void testDialectsAdvertiseRanges();
    void testDialectsReportServerError();
    void testDialectsSendRequestHeaders();

    void testDialectsEndAtContentLength();
    void testDialectsFollowRedirect();
    void testChunkedBodyWithTrailers();
    void testDialectsProxyForwarding();
    void testProxyAuthentication_data();
    void testProxyAuthentication();
    void testSocksProxy();
    void testResolveProxy();

private:
    TestHttpServer *m_server = nullptr;
};

void TestRadioStream::initTestCase()
{
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

void TestRadioStream::testLifecycle()
{
    RadioStream stream;
    QVERIFY(!stream.isAborted());

    const QUrl url1("http://127.0.0.1:8080/stream1"_L1);
    stream.setUrl(url1);
    QVERIFY(!stream.isAborted());

    const QUrl url2("http://127.0.0.1:8080/stream2"_L1);
    stream.setUrl(url2);
    QVERIFY(!stream.isAborted());

    stream.stop();
    QVERIFY(stream.isAborted());

    // Multiple start/stop cycles reset and set aborted flag cleanly
    for (int i = 0; i < 3; ++i) {
        stream.start();
        QVERIFY(!stream.isAborted());
        stream.stop();
        QVERIFY(stream.isAborted());
    }
}

void TestRadioStream::testReadBlocksUntilStop()
{
    RadioStream stream;
    stream.setUrl(QUrl("http://127.0.0.1:0/stream"_L1));

    std::atomic<bool> readCompleted{false};
    std::atomic<int> readResult{-2};
    std::vector<uint8_t> readBuffer(1024);

    std::thread readThread([&]() {
        std::stop_source stopSource;
        std::stop_token st = stopSource.get_token();

        readResult = stream.read(readBuffer, st);
        readCompleted = true;
    });

    QTest::qWait(50);

    QVERIFY(!readCompleted.load());

    stream.stop();

    readThread.join();

    QVERIFY(readCompleted.load());
    QCOMPARE(readResult.load(), -1);
}

void TestRadioStream::testErrorSignal()
{
    RadioStream stream;

    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(QUrl("http://invalid-domain-that-does-not-exist-12345.com/stream"_L1));
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 15000);

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

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "Rock & Roll - Don't Stop"_L1);
        QTest::newRow("unescaped-quote") << QByteArray("StreamTitle='Rock & Roll - Don't Stop';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, QStringLiteral("Café del Mar"));
        QTest::newRow("latin1-fallback") << QByteArray("StreamTitle='Caf\xE9 del Mar';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "A - B"_L1);
        QByteArray block = "StreamTitle='A - B';"_ba;
        block.append(QByteArray(12, '\0'));
        QTest::newRow("nul-padded") << block << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "Radio; Mix"_L1);
        QTest::newRow("semicolon-in-value") << QByteArray("StreamTitle='Radio; Mix';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "A - B"_L1);
        expected.insert("ServerGenre"_L1, "Rock"_L1);
        QTest::newRow("custom-field") << QByteArray("StreamTitle='A - B';ServerGenre='Rock';") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, "A - B"_L1);
        QTest::newRow("no-trailing-semicolon") << QByteArray("StreamTitle='A - B'") << expected;
    }

    {
        FieldHash expected;
        expected.insert("StreamTitle"_L1, QString::fromUtf8("Bj\xC3\xB6rk - Unison's Howling; Twice"));
        QByteArray block = "StreamTitle='Bj\xC3\xB6rk - Unison''s Howling; Twice';"_ba;
        block.append(QByteArray(64 - block.size(), '\0'));
        QTest::newRow("utf8-escaped-and-padded") << block << expected;
    }

    {
        FieldHash expected;
        QTest::newRow("unquoted-value-is-dropped") << QByteArray("StreamTitle=A - B;") << expected;
    }
}

void TestRadioStream::testMetadataParsing()
{
    QFETCH(QByteArray, metadata);
    QFETCH(FieldHash, expectedFields);

    qRegisterMetaType<DragonIcyMetadata>("DragonIcyMetadata");

    RadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    QSignalSpy metadataSpy(&stream, &RadioStream::metadataReady);
    QVERIFY(metadataSpy.isValid());

    stream.processMetadata(metadata);

    if (expectedFields.isEmpty()) {
        QCOMPARE(metadataSpy.count(), 0);
    } else {
        QCOMPARE(metadataSpy.count(), 1);

        const auto emitted = qvariant_cast<DragonIcyMetadata>(metadataSpy.takeFirst().at(0));

        if (expectedFields.contains("StreamTitle"_L1)) {
            QVERIFY2(!emitted.streamTitle().isEmpty(), "Expected StreamTitle to be set");
            QCOMPARE(emitted.streamTitle(), expectedFields.value("StreamTitle"_L1));
        }

        if (expectedFields.contains("StreamUrl"_L1)) {
            QVERIFY2(!emitted.streamUrl().isEmpty(), "Expected StreamUrl to be set");
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

void TestRadioStream::testReadBehaviorWithStopToken()
{
    RadioStream stream;
    stream.setUrl(QUrl("http://127.0.0.1:0/stream"_L1));

    std::vector<uint8_t> buffer(1024);
    std::atomic<bool> readCompleted{false};
    std::atomic<int> readResult{-2};

    std::stop_source source;
    std::stop_token token = source.get_token();

    std::thread readThread([&]() {
        readResult = stream.read(buffer, token);
        readCompleted = true;
    });

    QTest::qWait(50);

    source.request_stop();

    readThread.join();

    QVERIFY(readCompleted.load());
    QVERIFY2(readResult.load() <= 0, qPrintable(u"Read should return <=0 after stop token cancellation, got %1"_s.arg(readResult.load())));
}

void TestRadioStream::testIsAbortedFlag()
{
    RadioStream stream;
    QVERIFY(!stream.isAborted());

    stream.stop();
    QVERIFY(stream.isAborted());

    stream.stop();
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

    RadioStream stream;
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

    RadioStream stream;
    stream.setUrl(url);

    QSignalSpy seekableSpy(&stream, &Stream::seekableChanged);
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

    RadioStream stream;
    QSignalSpy bufferingSpy(&stream, &RadioStream::streamBuffering);
    QSignalSpy bufferedSpy(&stream, &RadioStream::streamBuffered);

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

    // Pause reader so buffer can refill above HIGH_WATER_MARK
    phase.store(1, std::memory_order_release);

    // Wait for buffer to refill above HIGH_WATER_MARK and streamBuffered to fire exactly once
    QTRY_COMPARE_WITH_TIMEOUT(bufferedSpy.count(), 1, 10000);

    // Resume reading to consume more data
    phase.store(0, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(totalRead.load() > 262144, 10000);

    QVERIFY2(totalRead.load() > 0, "Should have read some data");
}

void TestRadioStream::testBackpressure()
{
    // Send a payload larger than the cap (~5.2 MiB) with a dribbling server.
    // Verifies:
    // 1. Buffer caps at MAX_BUFFER_BYTES
    // 2. Backpressure suspends socket reads
    // 3. Resuming the reader resumes transport delivery
    // 4. Tail held by backpressure drains on EOF
    // 5. All data arrives without loss
    const QByteArray payload = QByteArray("0123456789").repeated(524288); // ~5.2 MiB

    QTcpServer server;
    QList<QTcpSocket *> clients;

    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *sock = server.nextPendingConnection();
            clients.append(sock);

            sock->readAll();

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
                const int len = static_cast<int>(std::min<qsizetype>(65536, payload.size() - *offset));
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

    RadioStream stream;
    stream.setUrl(url);
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(stream.bufferDepth() >= RadioStream::MAX_BUFFER_BYTES, 15000);
    QVERIFY2(stream.bufferDepth() <= RadioStream::MAX_BUFFER_BYTES + 65536, qPrintable(u"Buffer depth %1 exceeds cap + tolerance"_s.arg(stream.bufferDepth())));

    // Read everything with a brief pause in the middle to exercise the suspend/resume cycle.
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
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

    QTRY_VERIFY_WITH_TIMEOUT(gotEof.load(), 30000);
    reader.join();

    QVERIFY(gotEof.load());
    QCOMPARE(received.size(), payload.size());
    QCOMPARE(received, payload);

    stream.stop();
    for (auto *c : clients) {
        c->deleteLater();
    }
}

namespace
{
/** Deterministic audio, so a test can compare every byte the decoder should see. */
QByteArray makeBody(qsizetype size)
{
    QByteArray body;
    body.resize(size);
    for (qsizetype i = 0; i < size; ++i) {
        body[i] = static_cast<char>((i * 7 + 3) & 0xFF);
    }
    return body;
}

/** Reads a stream from a worker thread, the way the decode pipeline does. */
class StreamCollector
{
public:
    explicit StreamCollector(RadioStream &stream)
        : m_stream(stream)
        , m_reader([this]() {
            collect();
        })
    {
    }

    ~StreamCollector()
    {
        m_stop.request_stop();
        if (m_reader.joinable()) {
            m_reader.join();
        }
    }

    StreamCollector(const StreamCollector &) = delete;
    StreamCollector &operator=(const StreamCollector &) = delete;

    [[nodiscard]] qsizetype total() const
    {
        return m_total.load();
    }

    [[nodiscard]] bool sawEndOfStream() const
    {
        return m_end.load();
    }

    [[nodiscard]] bool sawFailure() const
    {
        return m_failure.load();
    }

    [[nodiscard]] QByteArray payload() const
    {
        std::scoped_lock lock(m_mutex);
        return m_payload;
    }

private:
    void collect()
    {
        std::vector<uint8_t> buffer(4096);
        while (!m_stop.stop_requested()) {
            const int read = m_stream.read(buffer, m_stop.get_token());
            if (read == 0) {
                m_end = true;
                return;
            }
            if (read < 0) {
                m_failure = true;
                return;
            }
            {
                std::scoped_lock lock(m_mutex);
                m_payload.append(reinterpret_cast<const char *>(buffer.data()), read);
            }
            m_total += read;
        }
    }

    RadioStream &m_stream;
    std::stop_source m_stop;
    mutable std::mutex m_mutex;
    QByteArray m_payload;
    std::atomic<qsizetype> m_total{0};
    std::atomic<bool> m_end{false};
    std::atomic<bool> m_failure{false};
    std::thread m_reader;
};

enum DialectColumn {
    HttpDialect,
    ShoutcastDialect,
};

void addDialectRows()
{
    QTest::addColumn<int>("dialect");
    QTest::newRow("http") << int(HttpDialect);
    QTest::newRow("shoutcast") << int(ShoutcastDialect);
}

StreamTestServer::Dialect dialectOf(int column)
{
    return column == ShoutcastDialect ? StreamTestServer::Dialect::Shoutcast : StreamTestServer::Dialect::Http;
}
}

// The same station, answered as modern HTTP and as SHOUTcast DNAS, has to behave
// the same way: Qt's HTTP stack reads the first and our own socket the second,
// and the decoder must not be able to tell which one it was fed by.
void TestRadioStream::testDialectsPlayAudio_data()
{
    addDialectRows();
}

void TestRadioStream::testDialectsPlayAudio()
{
    QFETCH(int, dialect);
    static constexpr int Metaint = 1024;

    const QByteArray audio = makeBody(3 * Metaint);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(dialectOf(dialect));
    server.setBody(audio);
    server.setIcyMetadata(Metaint, "Slay Test");

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);
    QSignalSpy metadataSpy(&stream, &RadioStream::metadataReady);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);

    // The metadata blocks never reach the audio, and no error surfaced on the way.
    QCOMPARE(collector.payload(), audio);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(metadataSpy.count(), 1);
    const auto icy = qvariant_cast<DragonIcyMetadata>(metadataSpy.at(0).at(0));
    QCOMPARE(icy.streamTitle(), QStringLiteral("Slay Test"));

    // However the server spelled its status line, the station was asked once:
    // no retries against a station that cannot be read the first time.
    QCOMPARE(server.connectionCount(), 1);

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsReportSize()
{
    const QByteArray audio = makeBody(4096);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Http);
    server.setBody(audio);

    RadioStream stream;
    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.sawEndOfStream(), 15000);

    // A station that says how much it is sending gets a duration
    QCOMPARE(stream.size(), audio.size());

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsEndOnServerClose()
{
    const QByteArray audio = makeBody(4096);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Shoutcast);
    server.setBody(audio);
    // A live station: it sends what it has and holds the connection open.
    server.setCloseAfterBody(false);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);
    QVERIFY(!collector.sawEndOfStream());

    // Hanging up is the normal end of a live stream, not a failure.
    server.hangUp();
    QTRY_VERIFY_WITH_TIMEOUT(collector.sawEndOfStream(), 15000);
    QCOMPARE(errorSpy.count(), 0);

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsAdvertiseRanges_data()
{
    QTest::addColumn<bool>("advertisesRanges");
    QTest::newRow("with ranges") << true;
    QTest::newRow("without ranges") << false;
}

void TestRadioStream::testDialectsAdvertiseRanges()
{
    QFETCH(bool, advertisesRanges);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Http);
    server.setBody(makeBody(4096));
    if (advertisesRanges) {
        server.setHeader("Accept-Ranges", "bytes");
    }

    RadioStream stream;
    QSignalSpy seekableSpy(&stream, &RadioStream::seekableChanged);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), 4096, 15000);

    QCOMPARE(stream.isSeekable(), advertisesRanges);
    QCOMPARE(seekableSpy.count(), advertisesRanges ? 1 : 0);
    if (advertisesRanges) {
        QVERIFY(seekableSpy.at(0).at(0).toBool());
    } else {
        // A station that never offered Range requests cannot be seeked.
        QCOMPARE(stream.seek(1024), qint64(-1));
    }

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsReportServerError()
{
    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Shoutcast);
    server.setStatus(404, "Not Found");

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.sawFailure(), 15000);

    QCOMPARE(errorSpy.count(), 1);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("404")), qPrintable(errorSpy.at(0).at(0).toString()));

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsSendRequestHeaders()
{
    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Http);
    server.setBody(makeBody(1024));

    RadioStream stream;
    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), 1024, 15000);

    // However the response arrives, the request asked for metadata the same way.
    const QByteArray request = server.lastRequest();
    QVERIFY2(request.startsWith("GET /stream HTTP/1."), request.constData());
    QVERIFY2(request.contains("Icy-Metadata: 1"), request.constData());
    QVERIFY2(request.contains("User-Agent: DragonMediaBackend/"), request.constData());

    stream.stop();
    server.stop();
}

void TestRadioStream::testChunkedBodyWithTrailers()
{
    const QByteArray audio = makeBody(8192);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setChunkedBody(true);
    server.setBody(audio);
    server.setChunkedTrailer("X-Ice-Public: yes\r\n");
    server.setCloseAfterBody(false);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.sawEndOfStream(), 15000);

    QCOMPARE(collector.payload(), audio);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(server.connectionCount(), 1);

    stream.stop();
    server.stop();
}

void TestRadioStream::testSeekRejectedWhenRangeIsIgnored()
{
    const QByteArray audio = makeBody(8192);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setBody(audio);
    // The server claims ranges but never honors one: it always answers 200
    // with the whole body. Seeking it must be refused rather than corrupt the
    // decoder's position.
    server.setHeader("Accept-Ranges"_ba, "bytes"_ba);
    server.setCloseAfterBody(false);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.total() >= 4096, 15000);
    QVERIFY(stream.isSeekable());

    QCOMPARE(stream.seek(4096), 4096);

    // The seek fails asynchronously: a full-body answer to a ranged request
    // is a protocol error, not audio at the new position.
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 15000);
    QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("range"), Qt::CaseInsensitive), qPrintable(errorSpy.at(0).at(0).toString()));
    QCOMPARE(server.connectionCount(), 2);

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsEndAtContentLength()
{
    const QByteArray audio = makeBody(6000);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Http);
    server.setBody(audio);
    server.setCloseAfterBody(false);
    server.setContentLengthHeader(true);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.sawEndOfStream(), 15000);

    QCOMPARE(collector.payload(), audio);
    QCOMPARE(stream.size(), audio.size());
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(server.connectionCount(), 1);

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsFollowRedirect()
{
    const QByteArray audio = makeBody(2048);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setDialect(StreamTestServer::Dialect::Shoutcast);
    server.setBody(audio);
    server.setRedirect("/elsewhere");

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);

    QCOMPARE(collector.payload(), audio);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(server.connectionCount(), 2);
    QVERIFY2(server.lastRequest().startsWith("GET /elsewhere"), server.lastRequest().constData());

    stream.stop();
    server.stop();
}

void TestRadioStream::testDialectsProxyForwarding()
{
    const QByteArray audio = makeBody(3000);

    StreamTestServer proxy;
    QVERIFY(proxy.start());
    proxy.setDialect(StreamTestServer::Dialect::Http);
    proxy.setBody(audio);

    ScopedProxyOverride proxyOverride(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), proxy.port()));

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    // A host only the proxy could ever reach.
    stream.setUrl(QUrl(QStringLiteral("http://radio.invalid/stream")));
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);

    QVERIFY2(proxy.lastRequest().startsWith("GET http://radio.invalid/stream HTTP/1.1"), proxy.lastRequest().constData());
    QCOMPARE(collector.payload(), audio);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(proxy.connectionCount(), 1);

    stream.stop();
    proxy.stop();
}

void TestRadioStream::testProxyAuthentication_data()
{
    QTest::addColumn<bool>("providesCredentials");
    QTest::newRow("without credentials") << false;
    QTest::newRow("with credentials") << true;
}

void TestRadioStream::testProxyAuthentication()
{
    QFETCH(bool, providesCredentials);

    const QByteArray audio = makeBody(1024);

    StreamTestServer proxy;
    QVERIFY(proxy.start());
    proxy.setBody(audio);
    proxy.setProxyCredentials("listener", "s3cret");

    QNetworkProxy configured(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), proxy.port());
    if (providesCredentials) {
        configured.setUser(QStringLiteral("listener"));
        configured.setPassword(QStringLiteral("s3cret"));
    }

    ScopedProxyOverride proxyOverride(configured);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(QUrl(QStringLiteral("http://radio.invalid/stream")));
    stream.start();

    StreamCollector collector(stream);

    if (providesCredentials) {
        // Credentials configured with the proxy are offered up front, so the
        // stream starts without an extra round trip.
        QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);
        QCOMPARE(errorSpy.count(), 0);
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 15000);
        QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("proxy"), Qt::CaseInsensitive), qPrintable(errorSpy.at(0).at(0).toString()));
    }

    // A proxy that refuses us is not something to hammer.
    QCOMPARE(proxy.connectionCount(), 1);

    stream.stop();
    proxy.stop();
}

void TestRadioStream::testSocksProxy()
{
    const QByteArray audio = makeBody(3000);

    StreamTestServer proxy;
    QVERIFY(proxy.start());
    proxy.setSocksProxy(true);
    proxy.setDialect(StreamTestServer::Dialect::Http);
    proxy.setBody(audio);

    ScopedProxyOverride proxyOverride(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), proxy.port()));

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    // Host and port that only the proxy could reach/resolve
    stream.setUrl(QUrl(QStringLiteral("http://radio.invalid:8000/stream")));
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), audio.size(), 15000);

    // SOCKS proxy receives domain name and port in CONNECT command
    QCOMPARE(proxy.lastSocksHost(), QByteArrayLiteral("radio.invalid"));
    QCOMPARE(proxy.lastSocksPort(), 8000);

    // Tunneled HTTP request uses origin form (/stream) and has no Proxy-Authorization header
    QVERIFY2(proxy.lastRequest().startsWith("GET /stream HTTP/1.1"), proxy.lastRequest().constData());
    QVERIFY(!proxy.lastRequest().contains("Proxy-Authorization"));

    QCOMPARE(collector.payload(), audio);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(proxy.connectionCount(), 1);

    stream.stop();
    proxy.stop();
}

void TestRadioStream::testResolveProxy()
{
    struct ListProxyFactory : public QNetworkProxyFactory {
        QList<QNetworkProxy> proxies;
        QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
        {
            return proxies;
        }
    };

    auto *factory = new ListProxyFactory();
    ScopedProxyOverride proxyOverride(factory);

    QNetworkProxy cachingHttp(QNetworkProxy::HttpCachingProxy, u"10.0.0.4"_s, 8080);
    cachingHttp.setCapabilities(QNetworkProxy::CachingCapability); // Non-tunneling
    QNetworkProxy socks(QNetworkProxy::Socks5Proxy, u"10.0.0.5"_s, 1080);

    // Cleartext HTTP accepts a non-tunneling forwarding proxy; HTTPS skips it for a tunneling one
    factory->proxies = {cachingHttp, socks};
    QCOMPARE(HttpProtocol::resolveProxy(QUrl(u"http://radio.example.org/stream"_s)).hostName(), u"10.0.0.4"_s);
    QCOMPARE(HttpProtocol::resolveProxy(QUrl(u"https://radio.example.org/stream"_s)).hostName(), u"10.0.0.5"_s);

    // HTTPS with only a non-tunneling proxy falls back to NoProxy
    factory->proxies = {cachingHttp};
    QCOMPARE(HttpProtocol::resolveProxy(QUrl(u"https://radio.example.org/stream"_s)).type(), QNetworkProxy::NoProxy);

    // Workaround for Qt's socks5h bug: Socks5Proxy missing TunnelingCapability is accepted and repaired
    QNetworkProxy socks5h(QNetworkProxy::Socks5Proxy, u"10.0.0.3"_s, 1080);
    socks5h.setCapabilities(QNetworkProxy::HostNameLookupCapability);
    factory->proxies = {socks5h};
    const QNetworkProxy resolved = HttpProtocol::resolveProxy(QUrl(u"https://radio.example.org/stream"_s));
    QCOMPARE(resolved.type(), QNetworkProxy::Socks5Proxy);
    QVERIFY(resolved.capabilities().testFlag(QNetworkProxy::TunnelingCapability));
}

void TestRadioStream::testIcyResponseParsing()
{
    // SHOUTcast DNAS answers listeners with the pre-standard "ICY 200 OK" status
    // line; reading it is the whole reason the stream drives a socket of its own.
    {
        const auto result = HttpProtocol::parseIcyResponse("ICY 200 OK\r\nicy-metaint:1024\r\nICY-Name: Test Radio");
        QVERIFY(result.has_value());
        QCOMPARE(result->statusCode, 200);
        QCOMPARE(result->headers.value("icy-metaint").toByteArray(), QByteArrayLiteral("1024"));
        QCOMPARE(result->headers.value("icy-name").toByteArray(), QByteArrayLiteral("Test Radio"));
    }

    // Ordinary HTTP is read too, and header names are matched regardless of case.
    {
        const auto result = HttpProtocol::parseIcyResponse("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg");
        QVERIFY(result.has_value());
        QCOMPARE(result->statusCode, 200);
        QCOMPARE(result->headers.value("content-TYPE").toByteArray(), QByteArrayLiteral("audio/mpeg"));
    }

    // A failed read carries the offending status line, for the caller's message.
    {
        const auto result = HttpProtocol::parseIcyResponse(QByteArray());
        QVERIFY(!result.has_value());
        QCOMPARE(static_cast<int>(result.error().error), static_cast<int>(HttpProtocol::IcyParseError::MalformedStatusLine));
    }
    {
        const auto result = HttpProtocol::parseIcyResponse("220 some FTP server\r\n");
        QVERIFY(!result.has_value());
        QCOMPARE(static_cast<int>(result.error().error), static_cast<int>(HttpProtocol::IcyParseError::NotAudioResponse));
        QCOMPARE(result.error().statusLine, QByteArrayLiteral("220 some FTP server"));
    }
    {
        const auto result = HttpProtocol::parseIcyResponse("ICY nothing here");
        QVERIFY(!result.has_value());
        QCOMPARE(static_cast<int>(result.error().error), static_cast<int>(HttpProtocol::IcyParseError::NonNumericStatus));
        QCOMPARE(result.error().statusLine, QByteArrayLiteral("ICY nothing here"));
    }
}

void TestRadioStream::testBodyFramerDirect()
{
    // Unframed: everything passes through, and the hang-up alone ends the body.
    {
        HttpBodyFramer framer;
        framer.start(false, -1);
        const auto out = framer.feed("hello"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("hello"));
        QVERIFY(!framer.isComplete());
        QVERIFY(!framer.isTruncated());
    }

    // Fixed length: bytes past the declared length are not part of the body.
    {
        HttpBodyFramer framer;
        framer.start(false, 5);
        const auto out = framer.feed("hello world"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("hello"));
        QVERIFY(framer.isComplete());
        QVERIFY(!framer.isTruncated());
    }
    {
        HttpBodyFramer framer;
        framer.start(false, 10);
        const auto out = framer.feed("hel"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("hel"));
        QVERIFY(!framer.isComplete());
        QVERIFY(framer.isTruncated()); // still owed bytes when the transport ended
    }

    // Chunked: extensions on the size line, a CRLF split across feeds, trailers.
    {
        HttpBodyFramer framer;
        framer.start(true, -1);
        auto out = framer.feed("4;ext=val\r\nWiki"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("Wiki"));
        out = framer.feed("\r\n5\r\npedia\r"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("pedia"));
        out = framer.feed("\n0\r\nX-Trailer: yes\r\n\r\n"_ba);
        QVERIFY(out.has_value());
        QVERIFY(framer.isComplete());
        QVERIFY(!framer.isTruncated());
    }

    // A chunk size line that does not parse is a malformed body.
    {
        HttpBodyFramer framer;
        framer.start(true, -1);
        const auto out = framer.feed("zz\r\n"_ba);
        QVERIFY(!out.has_value());
        QCOMPARE(out.error(), HttpBodyFramer::Error::MalformedChunkSize);
        QVERIFY(framer.isTruncated());
    }

    // The response headers pick the framing.
    {
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::TransferEncoding, "chunked"_ba);
        HttpBodyFramer framer;
        framer.startFromHeaders(headers);
        const auto out = framer.feed("3\r\nabc\r\n0\r\n\r\n"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("abc"));
        QVERIFY(framer.isComplete());
    }
    {
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ContentLength, "2"_ba);
        HttpBodyFramer framer;
        framer.startFromHeaders(headers);
        const auto out = framer.feed("abcd"_ba);
        QVERIFY(out.has_value());
        QCOMPARE(*out, QByteArrayLiteral("ab"));
        QVERIFY(framer.isComplete());
    }

    // Unbounded chunk size line or trailer is rejected with MalformedChunkSize
    {
        HttpBodyFramer framer;
        framer.start(true, -1);
        const QByteArray hugeChunkLine(HttpBodyFramer::MAX_CHUNK_LINE_BYTES + 1, '1');
        const auto out = framer.feed(hugeChunkLine);
        QVERIFY(!out.has_value());
        QCOMPARE(out.error(), HttpBodyFramer::Error::MalformedChunkSize);
        QVERIFY(framer.isTruncated());
    }
    {
        HttpBodyFramer framer;
        framer.start(true, -1);
        // Feed chunk 0 to enter trailer state
        const auto out1 = framer.feed("0\r\n"_ba);
        QVERIFY(out1.has_value());
        const QByteArray hugeTrailer(HttpBodyFramer::MAX_CHUNK_LINE_BYTES + 1, 'X');
        const auto out2 = framer.feed(hugeTrailer);
        QVERIFY(!out2.has_value());
        QCOMPARE(out2.error(), HttpBodyFramer::Error::MalformedChunkSize);
        QVERIFY(framer.isTruncated());
    }
    // reset() clears framing and pending allocations
    {
        HttpBodyFramer framer;
        framer.start(true, -1);
        framer.reset();
        QVERIFY(!framer.isComplete());
        QVERIFY(!framer.isTruncated());
    }
}

void TestRadioStream::testStopCancelsPendingReconnectTimer()
{
    // A server that accepts and immediately hangs up: a retryable failure that
    // starts the reconnect timer.
    QTcpServer hangingUpServer;
    QVERIFY(hangingUpServer.listen(QHostAddress::LocalHost));
    int hangups = 0;
    QObject::connect(&hangingUpServer, &QTcpServer::newConnection, &hangingUpServer, [&]() {
        ++hangups;
        while (hangingUpServer.hasPendingConnections()) {
            delete hangingUpServer.nextPendingConnection();
        }
    });

    // A server that answers like a live SHOUTcast stream and stays connected.
    QTcpServer liveServer;
    QVERIFY(liveServer.listen(QHostAddress::LocalHost));
    int liveConnections = 0;
    QObject::connect(&liveServer, &QTcpServer::newConnection, &liveServer, [&]() {
        ++liveConnections;
        while (liveServer.hasPendingConnections()) {
            QTcpSocket *client = liveServer.nextPendingConnection();
            client->write("ICY 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n");
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    RadioStream stream;
    QSignalSpy stallSpy(&stream, &RadioStream::streamStalled);

    stream.setUrl(QUrl(u"http://127.0.0.1:%1/"_s.arg(hangingUpServer.serverPort())));
    stream.start();

    // A retryable failure has been seen, so the reconnect timer is running.
    QTRY_VERIFY_WITH_TIMEOUT(stallSpy.size() >= 1, 10000);

    // The stream is replaced within the 2-second reconnect window; the timer
    // that was scheduled for the old connection must not fire restart() under
    // the new one.
    stream.stop();
    stream.setUrl(QUrl(u"http://127.0.0.1:%1/"_s.arg(liveServer.serverPort())));
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(liveConnections >= 1, 10000);
    QTest::qWait(2500);
    QCOMPARE(liveConnections, 1);

    stream.stop();
    hangingUpServer.close();
    liveServer.close();
}

void TestRadioStream::testInformational1xxWithPipelinedResponse()
{
    // Pipelined 100 Continue and 200 OK in the same read must not return early and strand the body
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *client = server.nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client, [client]() {
                client->readAll();
                client->write("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
                client->flush();
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    RadioStream stream;
    stream.setUrl(QUrl(u"http://127.0.0.1:%1/stream"_s.arg(server.serverPort())));
    stream.start();

    StreamCollector collector(stream);
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), 5, 2000);
    QCOMPARE(collector.payload(), QByteArrayLiteral("hello"));

    stream.stop();
    server.close();
}

void TestRadioStream::testEndStreamReentrancy()
{
    // Re-opening the socket during completion in bodyDataAvailable must not be clobbered by close() in endStream()
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QList<QTcpSocket *> clients;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *client = server.nextPendingConnection();
            clients.append(client);
            QObject::connect(client, &QTcpSocket::readyRead, client, [client]() {
                const QByteArray req = client->readAll();
                if (req.contains("GET /first")) {
                    client->write("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nhello");
                    client->flush();
                    client->disconnectFromHost();
                } else if (req.contains("GET /second")) {
                    client->write("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nworld");
                    client->flush();
                    // Keep connection open for /second
                }
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    HttpSocket socket;
    QSignalSpy finishedSpy(&socket, &HttpSocket::finished);

    const QUrl secondUrl(u"http://127.0.0.1:%1/second"_s.arg(server.serverPort()));
    bool reopened = false;

    QObject::connect(&socket, &HttpSocket::bodyDataAvailable, &socket, [&](QByteArrayView) {
        if (!reopened) {
            reopened = true;
            socket.open(secondUrl);
        }
    });

    socket.open(QUrl(u"http://127.0.0.1:%1/first"_s.arg(server.serverPort())));

    QTRY_VERIFY_WITH_TIMEOUT(reopened, 5000);
    // In buggy code, close() in endStream() runs after bodyDataAvailable and aborts the new connection,
    // setting state to Idle and emitting finished() for the old stream.
    QVERIFY2(socket.isActive(), "Socket should remain active after re-opening in bodyDataAvailable callback");
    QCOMPARE(finishedSpy.size(), 0);

    socket.close();
    server.close();
}

void TestRadioStream::testIcyDemuxerReentrancy()
{
    // Re-entrancy during feedInterleaved (e.g. reset/clearPendingBytes inside onAudio/onMetadata)
    // must not cause use-after-free, dangling views, or negative length removal on pending buffer.
    IcyDemuxer demuxer;
    demuxer.setIcyMetaInt(4);

    // Stream: 4 bytes audio ("1234"), 16 bytes metadata (len byte = 1, "StreamTitle='A';"), 4 bytes audio ("5678"), 16 bytes metadata ("StreamTitle='B';")
    const QByteArray data = "1234\x01StreamTitle='A';5678\x01StreamTitle='B';"_ba;

    int metaCalls = 0;
    int audioCalls = 0;
    demuxer.feed(
        data,
        [&](QByteArrayView) {
            ++audioCalls;
        },
        [&](const DragonIcyMetadata &) {
            ++metaCalls;
            // Consumer resets demuxer for a new stream upon receiving first metadata block
            demuxer.reset(4);
        });

    // In fixed code, demuxer aborts loop immediately when reset during callback,
    // discarding obsolete remaining data and not calling callbacks again:
    QCOMPARE(metaCalls, 1);
    QCOMPARE(audioCalls, 1);

    // Verify demuxer state is clean and can process a new stream without corruption:
    demuxer.setIcyMetaInt(4);
    int newAudioCalls = 0;
    demuxer.feed(
        "WXYZ"_ba,
        [&](QByteArrayView audio) {
            ++newAudioCalls;
            QCOMPARE(audio, "WXYZ"_ba);
        },
        [](const DragonIcyMetadata &) { });
    QCOMPARE(newAudioCalls, 1);

    // Verify re-entrancy from within onAudio (e.g. seeking or clearing during audio delivery):
    demuxer.reset(4);
    int audioCalls2 = 0;
    int metaCalls2 = 0;
    demuxer.feed(
        data,
        [&](QByteArrayView) {
            ++audioCalls2;
            demuxer.clearPendingBytes();
        },
        [&](const DragonIcyMetadata &) {
            ++metaCalls2;
        });
    QCOMPARE(audioCalls2, 1);
    QCOMPARE(metaCalls2, 0);
}

void TestRadioStream::testIcyDemuxerEmptyTitleClearing()
{
    // processBlock must not drop explicit StreamTitle=''; clearing updates
    IcyDemuxer demuxer;

    // Initial block with a song title
    const auto meta1 = demuxer.processBlock("StreamTitle='Song 1';\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"_ba);
    QVERIFY(meta1.has_value());
    QCOMPARE(meta1->streamTitle(), QStringLiteral("Song 1"));

    // Repeat of the same title is suppressed (deduplicated)
    const auto metaRepeat = demuxer.processBlock("StreamTitle='Song 1';\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"_ba);
    QVERIFY(!metaRepeat.has_value());

    // Station sends an explicit empty StreamTitle=''; to clear the metadata between songs.
    // In buggy code with `if (icy.isNull() || icy == m_lastMetadata)`, this is dropped because icy.isNull() is true!
    const auto metaClear = demuxer.processBlock("StreamTitle='';\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"_ba);
    QVERIFY2(metaClear.has_value(), "Explicit empty StreamTitle=''; block must not be dropped by icy.isNull()");
    QCOMPARE(metaClear->streamTitle(), QString());
}

void TestRadioStream::testIcyDemuxerSplitMetadata()
{
    // Tests that metadata block split across multiple feed() packet boundaries is assembled cleanly
    IcyDemuxer demuxer;
    demuxer.setIcyMetaInt(4);

    QByteArray meta = "StreamTitle='Song Title';"_ba;
    while (meta.size() % 16 != 0) {
        meta.append('\0');
    }
    const char lenByte = static_cast<char>(meta.size() / 16);

    QByteArray full = "1234"_ba;
    full.append(lenByte);
    full.append(meta);
    full.append("5678"_ba);

    const qsizetype splitPoint = 4 + 1 + 10;
    const QByteArray part1 = full.first(splitPoint);
    const QByteArray part2 = full.sliced(splitPoint);

    QByteArray audioReceived;
    QString titleReceived;

    demuxer.feed(
        part1,
        [&](QByteArrayView a) {
            audioReceived += a.toByteArray();
        },
        [&](const DragonIcyMetadata &m) {
            titleReceived = m.streamTitle();
        });

    QCOMPARE(audioReceived, "1234"_ba);
    QVERIFY(titleReceived.isEmpty());

    demuxer.feed(
        part2,
        [&](QByteArrayView a) {
            audioReceived += a.toByteArray();
        },
        [&](const DragonIcyMetadata &m) {
            titleReceived = m.streamTitle();
        });

    QCOMPARE(audioReceived, "12345678"_ba);
    QCOMPARE(titleReceived, QStringLiteral("Song Title"));
}

void TestRadioStream::testHttpRequestSetHeaderReplaces()
{
    // HttpRequest::setHeader must overwrite existing headers case-insensitively instead of appending duplicates
    HttpProtocol::HttpRequest request("/stream"_ba, "example.com:8080"_ba);

    // Overwrite the constructor-set Host header
    request.setHeader("Host"_ba, "newhost.com:9090"_ba);

    // Set custom headers with case variations
    request.setHeader("User-Agent"_ba, "FirstAgent"_ba);
    request.setHeader("user-agent"_ba, "SecondAgent"_ba);

    const QByteArray wire = request.toByteArray();

    // Verify there is only one Host: and only one User-Agent: header
    QCOMPARE(wire.count("Host:"_ba), 1);
    QVERIFY(wire.contains("Host: newhost.com:9090\r\n"_ba));
    QVERIFY(!wire.contains("Host: example.com:8080\r\n"_ba));

    QCOMPARE(wire.count("user-agent:"_ba) + wire.count("User-Agent:"_ba), 1);
    QVERIFY(wire.contains("user-agent: SecondAgent\r\n"_ba) || wire.contains("User-Agent: SecondAgent\r\n"_ba));
    QVERIFY(!wire.contains("FirstAgent"_ba));

    // Verify buildStreamGetRequest moves target and produces valid request
    const QUrl url(u"https://example.com/live"_s);
    const QByteArray req = HttpProtocol::buildStreamGetRequest(url, QNetworkProxy{QNetworkProxy::NoProxy}, std::nullopt);
    QVERIFY(req.startsWith("GET /live HTTP/1.1\r\n"_ba));
    QVERIFY(req.contains("Host: example.com\r\n"_ba));
}

void TestRadioStream::testStreamStalledStopCancelsReconnect()
{
    // When a slot connected to streamStalled synchronously calls stream.stop(),
    // handleTransportFailed must not re-arm m_reconnectTimer afterward.
    QTcpServer hangingUpServer;
    QVERIFY(hangingUpServer.listen(QHostAddress::LocalHost));
    QObject::connect(&hangingUpServer, &QTcpServer::newConnection, &hangingUpServer, [&]() {
        while (hangingUpServer.hasPendingConnections()) {
            delete hangingUpServer.nextPendingConnection();
        }
    });

    RadioStream stream;
    bool stalledReceived = false;
    QObject::connect(&stream, &RadioStream::streamStalled, &stream, [&]() {
        stalledReceived = true;
        stream.stop();
    });

    stream.setUrl(QUrl(u"http://127.0.0.1:%1/"_s.arg(hangingUpServer.serverPort())));
    stream.start();

    QTRY_VERIFY_WITH_TIMEOUT(stalledReceived, 5000);

    // Give event loop processing a moment to settle
    QTest::qWait(50);

    // In buggy code, m_reconnectTimer->start() was executed AFTER stream.stop() stopped it,
    // so the timer would be active despite stop() being called.
    QVERIFY2(!stream.m_reconnectTimer.isActive(), "m_reconnectTimer must not be active after stop() called from streamStalled slot");
    QVERIFY(stream.isAborted());

    hangingUpServer.close();
}

void TestRadioStream::testContentRangeParsing()
{
    // Valid standard Content-Range
    {
        const auto res = HttpProtocol::parseContentRange("bytes 0-1023/2048"_ba);
        QVERIFY(res.has_value());
        QCOMPARE(res->first, 0);
        QCOMPARE(res->last, 1023);
        QCOMPARE(res->total, 2048);
    }
    // Unknown total size with '*'
    {
        const auto res = HttpProtocol::parseContentRange("bytes 500-999/*"_ba);
        QVERIFY(res.has_value());
        QCOMPARE(res->first, 500);
        QCOMPARE(res->last, 999);
        QCOMPARE(res->total, -1);
    }
    // Case-insensitive unit and whitespace tolerance
    {
        const auto res = HttpProtocol::parseContentRange("  BYTES  100-200/5000  "_ba);
        QVERIFY(res.has_value());
        QCOMPARE(res->first, 100);
        QCOMPARE(res->last, 200);
        QCOMPARE(res->total, 5000);
    }
    // Malformed range strings
    QVERIFY(!HttpProtocol::parseContentRange(""_ba).has_value());
    QVERIFY(!HttpProtocol::parseContentRange("items 0-10/20"_ba).has_value());
    QVERIFY(!HttpProtocol::parseContentRange("bytes -10/20"_ba).has_value());
    QVERIFY(!HttpProtocol::parseContentRange("bytes 200-100/500"_ba).has_value()); // last < first
    QVERIFY(!HttpProtocol::parseContentRange("bytes 100-200"_ba).has_value());
}

void TestRadioStream::testHostHeaderAndRequestTarget()
{
    // Host header with default port vs custom port
    QCOMPARE(HttpProtocol::hostHeader(QUrl("http://example.com"_L1)), QByteArrayLiteral("example.com"));
    QCOMPARE(HttpProtocol::hostHeader(QUrl("http://example.com:8080"_L1)), QByteArrayLiteral("example.com:8080"));
    QCOMPARE(HttpProtocol::hostHeader(QUrl("https://example.com:443"_L1)), QByteArrayLiteral("example.com"));

    // Host header with IPv6
    QCOMPARE(HttpProtocol::hostHeader(QUrl("http://[::1]"_L1)), QByteArrayLiteral("[::1]"));
    QCOMPARE(HttpProtocol::hostHeader(QUrl("http://[::1]:8000"_L1)), QByteArrayLiteral("[::1]:8000"));

    QCOMPARE(HttpProtocol::requestTarget(QUrl("http://example.com"_L1)), QByteArrayLiteral("/"));
    QCOMPARE(HttpProtocol::requestTarget(QUrl("http://example.com/stream"_L1)), QByteArrayLiteral("/stream"));
    QCOMPARE(HttpProtocol::requestTarget(QUrl("http://example.com?radio=1"_L1)), QByteArrayLiteral("/?radio=1"));
    QCOMPARE(HttpProtocol::requestTarget(QUrl("http://example.com/live?codec=mp3#frag"_L1)), QByteArrayLiteral("/live?codec=mp3"));
}

void TestRadioStream::testSocketErrorAndRetryMapping()
{
    // SSL handshake failure mapping
    QCOMPARE(HttpProtocol::mapSocketError(QAbstractSocket::SslHandshakeFailedError), QNetworkReply::SslHandshakeFailedError);
    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::SslHandshakeFailedError));

    // Proxy errors mapping
    QCOMPARE(HttpProtocol::mapSocketError(QAbstractSocket::ProxyConnectionRefusedError), QNetworkReply::ProxyConnectionRefusedError);
    QCOMPARE(HttpProtocol::mapSocketError(QAbstractSocket::ProxyConnectionTimeoutError), QNetworkReply::ProxyTimeoutError);

    QVERIFY(HttpProtocol::isRetryable(QNetworkReply::TimeoutError));
    QVERIFY(HttpProtocol::isRetryable(QNetworkReply::RemoteHostClosedError));
    QVERIFY(HttpProtocol::isRetryable(QNetworkReply::ConnectionRefusedError));
    QVERIFY(HttpProtocol::isRetryable(QNetworkReply::HostNotFoundError));
    QVERIFY(HttpProtocol::isRetryable(QNetworkReply::ServiceUnavailableError));

    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::ContentNotFoundError));
    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::ContentAccessDenied));
    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::AuthenticationRequiredError));
    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::ProxyAuthenticationRequiredError));
    QVERIFY(!HttpProtocol::isRetryable(QNetworkReply::ProtocolFailure));
}

void TestRadioStream::testSeekToZeroWith200Ok()
{
    // When seeking to 0, server responding with 200 OK (whole file from byte 0) must be accepted
    const QByteArray audio = makeBody(4096);

    StreamTestServer server;
    QVERIFY(server.start());
    server.setBody(audio);
    server.setHeader("Accept-Ranges"_ba, "bytes"_ba);
    server.setCloseAfterBody(false);

    RadioStream stream;
    QSignalSpy errorSpy(&stream, &RadioStream::errorOccurred);

    stream.setUrl(server.url());
    stream.start();

    StreamCollector collector(stream);
    QTRY_VERIFY_WITH_TIMEOUT(collector.total() >= 2048, 15000);
    QVERIFY(stream.isSeekable());

    // Seeking to 0 sends Range: bytes=0-. Server responds with 200 OK with whole file.
    // This must succeed rather than fail with "Server ignored requested byte range".
    QCOMPARE(stream.seek(0), 0);
    QTest::qWait(200);
    QCOMPARE(errorSpy.count(), 0);

    stream.stop();
    server.stop();
}

void TestRadioStream::testTlsSessionTicketResumption()
{
    HttpSocket socket;

    // Initial ticket state is empty
    QVERIFY(socket.sessionTicket().isEmpty());
    QVERIFY(socket.sessionTicketHost().isEmpty());
    QCOMPARE(socket.sessionTicketPort(), 0);

    // Setting ticket directly updates cached fields
    socket.setSessionTicket("tls-ticket-sample-1234"_ba, "stream.example.org"_L1, 443);
    QCOMPARE(socket.sessionTicket(), "tls-ticket-sample-1234"_ba);
    QCOMPARE(socket.sessionTicketHost(), "stream.example.org"_L1);
    QCOMPARE(socket.sessionTicketPort(), 443);

    // Clear ticket resets all fields
    socket.clearSessionTicket();
    QVERIFY(socket.sessionTicket().isEmpty());
    QVERIFY(socket.sessionTicketHost().isEmpty());
    QCOMPARE(socket.sessionTicketPort(), 0);

    // Ticket is preserved and applied when re-opening the same URL (e.g. seek or reconnect)
    socket.setSessionTicket("tls-ticket-resumed"_ba, "stream.example.com"_L1, 443);
    socket.open(QUrl("https://stream.example.com/stream"_L1));
    QCOMPARE(socket.sessionTicket(), "tls-ticket-resumed"_ba);
    QCOMPARE(socket.sessionTicketHost(), "stream.example.com"_L1);
    QCOMPARE(socket.sessionTicketPort(), 443);

    // SslConfiguration on socket received the ticket
    auto *sslSocket = qobject_cast<QSslSocket *>(socket.m_socket.data());
    QVERIFY(sslSocket != nullptr);
    QCOMPARE(sslSocket->sslConfiguration().sessionTicket(), "tls-ticket-resumed"_ba);

    // Seeking on the same stream keeps the ticket
    socket.open(QUrl("https://stream.example.com/stream"_L1), 4096);
    QCOMPARE(socket.sessionTicket(), "tls-ticket-resumed"_ba);
    socket.close();

    // Switching to a different host/URL clears the ticket
    socket.setSessionTicket("tls-ticket-resumed"_ba, "stream.example.com"_L1, 443);
    socket.open(QUrl("https://other-server.org/live"_L1));
    QVERIFY(socket.sessionTicket().isEmpty());
    QVERIFY(socket.sessionTicketHost().isEmpty());
    QCOMPARE(socket.sessionTicketPort(), 0);
    socket.close();

    // SSL handshake failure clears the ticket
    socket.setSessionTicket("tls-ticket-handshake-fail"_ba, "stream.example.com"_L1, 443);
    socket.open(QUrl("https://stream.example.com/stream"_L1));
    QVERIFY(!socket.sessionTicket().isEmpty());
    if (socket.m_socket) {
        Q_EMIT socket.m_socket->errorOccurred(QAbstractSocket::SslHandshakeFailedError);
    }
    QVERIFY(socket.sessionTicket().isEmpty());
    socket.close();

    // RadioStream::setUrl clears the ticket when switching station URL
    RadioStream stream;
    stream.m_httpSocket->setSessionTicket("ticket-radiostream"_ba, "station-a.com"_L1, 443);
    stream.setUrl(QUrl("https://station-a.com/live"_L1));
    QCOMPARE(stream.m_httpSocket->sessionTicket(), "ticket-radiostream"_ba);
    stream.setUrl(QUrl("https://station-b.com/live"_L1));
    QVERIFY(stream.m_httpSocket->sessionTicket().isEmpty());
}

void TestRadioStream::testParseResponseHeadersDirect()
{
    using Socket = HttpSocket;

    // Malformed response
    {
        const auto res = Socket::parseResponseHeaders("GARBAGE NOT HTTP\r\n\r\n"_ba);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);
    }

    // Informational 1xx
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 100 Continue\r\n\r\n"_ba);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::InformationalResponse>(*res));
    }

    // 200 OK without range
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n"_ba);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::BodyResponse>(*res));
        const auto &body = std::get<Socket::BodyResponse>(*res);
        QCOMPARE(body.headers.value(QHttpHeaders::WellKnownHeader::ContentType), "audio/mpeg"_ba);
    }

    // 206 Partial Content matching range
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 500-1000/2000\r\n\r\n"_ba, 500);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::BodyResponse>(*res));
    }

    // 206 Partial Content with mismatched range
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 600-1000/2000\r\n\r\n"_ba, 500);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);
        QVERIFY(res.error().message.contains("unexpected byte range"_L1));
    }

    // 200 OK when rangeStart == 0
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 200 OK\r\n\r\n"_ba, 0);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::BodyResponse>(*res));
    }

    // 200 OK when rangeStart > 0 (server ignored range)
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 200 OK\r\n\r\n"_ba, 500);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);
        QVERIFY(res.error().message.contains("ignored requested byte range"_L1));
    }

    // 302 Found with Location
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 302 Found\r\nLocation: https://cdn.example.com/audio\r\n\r\n"_ba);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::RedirectResponse>(*res));
        QCOMPARE(std::get<Socket::RedirectResponse>(*res).location, "https://cdn.example.com/audio"_ba);
    }

    // 302 Found without Location
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 302 Found\r\n\r\n"_ba);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);
    }

    // 404 Not Found
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 404 Not Found\r\n\r\n"_ba);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ContentNotFoundError);
    }

    // 407 Proxy Authentication Required
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n"_ba);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ProxyAuthenticationRequiredError);
    }

    // 503 Service Unavailable
    {
        const auto res = Socket::parseResponseHeaders("HTTP/1.1 503 Service Unavailable\r\n\r\n"_ba);
        QVERIFY(!res.has_value());
        QCOMPARE(res.error().error, QNetworkReply::ServiceUnavailableError);
    }

    // ICY 200 OK
    {
        const auto res = Socket::parseResponseHeaders("ICY 200 OK\r\nicy-metaint: 8192\r\n\r\n"_ba);
        QVERIFY(res.has_value());
        QVERIFY(std::holds_alternative<Socket::BodyResponse>(*res));
        QCOMPARE(std::get<Socket::BodyResponse>(*res).headers.value("icy-metaint"_ba), "8192"_ba);
    }
}

void TestRadioStream::testHttpSocketRedirectErrors()
{
    HttpSocket socket;
    socket.m_initialUrl = QUrl("https://example.com/stream"_L1);
    socket.m_currentUrl = socket.m_initialUrl;

    // Insecure redirect (HTTPS -> HTTP)
    auto res = socket.followRedirect("http://example.com/insecure"_ba);
    QVERIFY(!res.has_value());
    QCOMPARE(res.error().error, QNetworkReply::InsecureRedirectError);

    // Unsupported scheme
    res = socket.followRedirect("ftp://example.com/audio.mp3"_ba);
    QVERIFY(!res.has_value());
    QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);

    // Max redirects exceeded
    socket.m_redirects = 5;
    res = socket.followRedirect("https://example.com/too_many"_ba);
    QVERIFY(!res.has_value());
    QCOMPARE(res.error().error, QNetworkReply::TooManyRedirectsError);

    // Empty location
    res = socket.followRedirect(""_ba);
    QVERIFY(!res.has_value());
    QCOMPARE(res.error().error, QNetworkReply::ProtocolFailure);
}

void TestRadioStream::testHttpSocketHeaderEdgeCases()
{
    // Header delimiter with bare LF (\n\n) over the socket: verify body is ingested
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                auto *client = server.nextPendingConnection();
                QObject::connect(client, &QTcpSocket::readyRead, client, [client]() {
                    client->readAll();
                    client->write("HTTP/1.1 200 OK\nContent-Type: audio/mpeg\nContent-Length: 5\n\nhello");
                    client->flush();
                });
                QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
            }
        });

        HttpSocket socket;
        QSignalSpy headersSpy(&socket, &HttpSocket::headersReceived);
        QByteArray receivedAudio;
        QObject::connect(&socket, &HttpSocket::bodyDataAvailable, &socket, [&](QByteArrayView audio) {
            receivedAudio += audio.toByteArray();
        });

        socket.open(QUrl(u"http://127.0.0.1:%1/stream"_s.arg(server.serverPort())));

        QTRY_COMPARE_WITH_TIMEOUT(headersSpy.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(receivedAudio, QByteArrayLiteral("hello"), 5000);

        socket.close();
        server.close();
    }

    // Oversized headers (> MAX_HEADER_BYTES) with no delimiter must fail with ProtocolFailure
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            while (server.hasPendingConnections()) {
                auto *client = server.nextPendingConnection();
                QObject::connect(client, &QTcpSocket::readyRead, client, [client]() {
                    client->readAll();
                    // Send > 64 KiB without any header delimiter
                    const QByteArray hugeHeaders(HttpSocket::MAX_HEADER_BYTES + 1024, 'A');
                    client->write(hugeHeaders);
                    client->flush();
                });
                QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
            }
        });

        HttpSocket socket;
        QSignalSpy failedSpy(&socket, &HttpSocket::failed);

        socket.open(QUrl(u"http://127.0.0.1:%1/stream"_s.arg(server.serverPort())));

        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(failedSpy.at(0).at(0).value<QNetworkReply::NetworkError>(), QNetworkReply::ProtocolFailure);

        socket.close();
        server.close();
    }

    // Invalid stream URL (empty host) fails immediately with ProtocolUnknownError without retries
    {
        HttpSocket socket;
        QSignalSpy failSpy(&socket, &HttpSocket::failed);
        socket.open(QUrl("http:///empty-host"_L1));
        QCOMPARE(failSpy.count(), 1);
        QCOMPARE(failSpy.at(0).at(0).value<QNetworkReply::NetworkError>(), QNetworkReply::ProtocolUnknownError);
        QCOMPARE(failSpy.at(0).at(2).toBool(), false);
    }

    // HttpRequest clearHeaders and userAgent
    {
        HttpProtocol::HttpRequest request("/stream"_ba, "example.com"_ba);
        request.clearHeaders();
        QCOMPARE(request.toByteArray(), "GET /stream HTTP/1.1\r\n\r\n"_ba);
        QVERIFY(!HttpProtocol::userAgent().isEmpty());
        QVERIFY(HttpProtocol::userAgent().startsWith("DragonMediaBackend/"));
    }
}

void TestRadioStream::testHttpSocketTruncatedBody()
{
    // Server claims 10000 bytes, sends 50 bytes and hangs up.
    // Framer detects truncation and socket fails with RemoteHostClosedError rather than emitting finished().
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *client = server.nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client, [client]() {
                client->readAll();
                client->write("HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\nHello");
                client->flush();
                client->disconnectFromHost();
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    HttpSocket socket;
    QSignalSpy failedSpy(&socket, &HttpSocket::failed);
    QSignalSpy finishedSpy(&socket, &HttpSocket::finished);

    socket.open(QUrl(u"http://127.0.0.1:%1/stream"_s.arg(server.serverPort())));

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
    QCOMPARE(failedSpy.at(0).at(0).value<QNetworkReply::NetworkError>(), QNetworkReply::RemoteHostClosedError);
    QCOMPARE(finishedSpy.count(), 0);

    socket.close();
    server.close();
}

void TestRadioStream::testRangeReconnectionOnDrop()
{
    // A seekable station (advertises Accept-Ranges: bytes) disconnects mid-stream.
    // The stream's reconnect should resume with Range: bytes=<position>-
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QByteArray firstRequest;
    QByteArray secondRequest;
    int connCount = 0;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        while (server.hasPendingConnections()) {
            auto *client = server.nextPendingConnection();
            ++connCount;
            QObject::connect(client, &QTcpSocket::readyRead, client, [client, &firstRequest, &secondRequest, connCount]() {
                const QByteArray req = client->readAll();
                if (connCount == 1) {
                    firstRequest = req;
                    client->write("HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nContent-Length: 4096\r\n\r\n" + QByteArray(2048, 'x'));
                    client->flush();
                    client->disconnectFromHost();
                } else if (connCount == 2) {
                    secondRequest = req;
                    client->write("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 2048-4095/4096\r\nContent-Length: 2048\r\nConnection: close\r\n\r\n"
                                  + QByteArray(2048, 'y'));
                    client->flush();
                    client->disconnectFromHost();
                }
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });

    RadioStream stream;
    stream.setUrl(QUrl(u"http://127.0.0.1:%1/stream"_s.arg(server.serverPort())));
    stream.start();

    StreamCollector collector(stream);
    // Reader should collect all 4096 bytes across the reconnect
    QTRY_COMPARE_WITH_TIMEOUT(collector.total(), 4096, 15000);

    QVERIFY(firstRequest.contains("GET /stream HTTP/1.1"));
    QVERIFY(!firstRequest.contains("Range:"));
    QVERIFY(secondRequest.contains("Range: bytes=2048-"));

    stream.stop();
    server.close();
}

#include "test_radiostream.moc"
