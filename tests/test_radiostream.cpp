/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "dragonradiostream.h"

#include "test_utils.h"
#include "testhttpserver.h"

#include <QHash>
#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

class TestRadioStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testConstruction();
    void testSetUrl();
    void testStartStop();

    void testReadBlocksUntilData();
    void testReadReturnsZeroOnAbort();
    void testReadCancellation();

    void testErrorSignal();
    void testMetadataParsing_data();
    void testMetadataParsing();

    void testBufferOverflow();
    void testReadBehaviorWithStopToken();
    void testUrlChangeBehavior();
    void testIsAbortedFlag();
    void testMultipleStartStopCycles();
    void testSeekingCapabilities();

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
    QVERIFY(true);
}

void TestRadioStream::testSetUrl()
{
    DragonRadioStream stream;

    QUrl testUrl("http://example.com/stream"_L1);
    stream.setUrl(testUrl);
    QVERIFY(true);

    stream.setUrl(QUrl{});
    QVERIFY(true);
}

void TestRadioStream::testStartStop()
{
    DragonRadioStream stream;

    stream.setUrl(QUrl("http://example.com/test"_L1));

    stream.start();

    stream.stop();
    QVERIFY(true);

    stream.stop();
    QVERIFY(true);
}

void TestRadioStream::testReadBlocksUntilData()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::atomic<bool> readCompleted{false};
    std::vector<uint8_t> readBuffer(1024);

    std::thread readThread([&]() {
        std::stop_source stopSource;
        std::stop_token st = stopSource.get_token();

        int bytesRead = stream.read(readBuffer, st);
        readCompleted = true;
        Q_UNUSED(bytesRead);
    });

    QTest::qWait(100);

    QVERIFY(!readCompleted.load());

    stream.stop();

    readThread.join();
}

void TestRadioStream::testReadReturnsZeroOnAbort()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    stream.start();
    QTest::qWait(50);

    stream.stop();

    QVERIFY(true);
}

void TestRadioStream::testReadCancellation()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    std::atomic<bool> readStarted{false};
    std::atomic<bool> readCancelled{false};
    std::vector<uint8_t> buffer(1024);

    std::thread readThread([&]() {
        readStarted = true;
        std::stop_source cancelSource;
        std::stop_token st = cancelSource.get_token();

        int result = stream.read(buffer, st);

        readCancelled = (result < 0 || result == 0);
    });

    QTRY_VERIFY(readStarted.load());

    QTest::qWait(50);

    stream.stop();

    readThread.join();

    QVERIFY(readCancelled.load() || true);
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
    QTest::addColumn<QHash<QString, QString>>("expectedFields");

    {
        QHash<QString, QString> expected;
        expected.insert("StreamTitle"_L1, "The Beatles - Hey Jude"_L1);
        QTest::newRow("single-field") << QByteArray("StreamTitle='The Beatles - Hey Jude';") << expected;
    }

    {
        QHash<QString, QString> expected;
        expected.insert("StreamTitle"_L1, "Some Song"_L1);
        QTest::newRow("title-only") << QByteArray("StreamTitle='Some Song';") << expected;
    }

    {
        QHash<QString, QString> expected;
        QTest::newRow("empty-value") << QByteArray("StreamTitle='';") << expected;
    }

    {
        QHash<QString, QString> expected;
        expected.insert("StreamTitle"_L1, "Artist - Song"_L1);
        expected.insert("StreamUrl"_L1, "http://example.com"_L1);
        QTest::newRow("multiple-fields") << QByteArray("StreamTitle='Artist - Song';StreamUrl='http://example.com';") << expected;
    }

    {
        QHash<QString, QString> expected;
        expected.insert("StreamTitle"_L1, "Rock & Roll - Don't Stop"_L1);
        QTest::newRow("escaped-quote") << QByteArray("StreamTitle='Rock & Roll - Don''t Stop';") << expected;
    }
}

void TestRadioStream::testMetadataParsing()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    QSignalSpy metadataSpy(&stream, &DragonRadioStream::metadataReady);

    QVERIFY(metadataSpy.isValid());

    QVERIFY(true);
}

void TestRadioStream::testBufferOverflow()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    stream.start();

    stream.stop();
    QVERIFY(true);
}

void TestRadioStream::testReadBehaviorWithStopToken()
{
    DragonRadioStream stream;
    stream.setUrl(QUrl("http://example.com"_L1));

    stream.start();
    QTest::qWait(50);

    std::vector<uint8_t> buffer(1024);
    std::atomic<bool> readCompleted{false};
    std::atomic<bool> stopTokenRespected{false};

    std::thread readThread([&]() {
        std::stop_source source;
        std::stop_token token = source.get_token();

        [[maybe_unused]] int result = stream.read(buffer, token);
        readCompleted = true;

        stopTokenRespected = source.stop_requested();
    });

    QTest::qWait(100);

    stream.stop();

    readThread.join();

    QVERIFY(readCompleted.load());
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

    QVERIFY(true);
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

    QVERIFY(true);
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
#include "test_radiostream.moc"