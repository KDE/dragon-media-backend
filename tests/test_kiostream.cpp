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

    void testHttpKioRead();
    void testHttpKioSeekDataCorrect();

    void testHttpKioSeekDoesNotCorruptStream();
    void testHttpKioRapidSeek();
    void testHttpKioSeekWhileReading();

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

    DragonKioStream stream;
    stream.setUrl(httpUrl(m_httpServer, u"/big.dat"_s));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    std::stop_token st = stopSource.get_token();

    std::atomic<int> firstRead{-2};
    std::jthread readThread([&]() {
        firstRead = stream.read(buffer, st);
    });

    QTest::qWait(100);

    stream.seek(1024);

    QTRY_VERIFY_WITH_TIMEOUT(firstRead.load() != -2, 5000);
    QVERIFY2(firstRead.load() >= 0, qPrintable(u"First read should have succeeded before seek, got %1"_s.arg(firstRead.load())));

    std::atomic<int> secondRead{-2};
    {
        std::jthread readThread2([&]() {
            secondRead = stream.read(buffer, st);
        });
        QTRY_VERIFY_WITH_TIMEOUT(secondRead.load() != -2, 5000);
    }

    QVERIFY2(secondRead.load() > 0, qPrintable(u"Stream unusable after seek-while-reading second read returned %1"_s.arg(secondRead.load())));

    const int seekOffset = 1024;
    for (int i = 0; i < secondRead.load(); ++i) {
        char expected = '0' + ((seekOffset + i) % 10);
        QVERIFY2(static_cast<char>(buffer[static_cast<size_t>(i)]) == expected,
                 qPrintable(u"Content mismatch at offset %1 (byte %2): expected '%3', got '%4'"_s.arg(seekOffset + i)
                                .arg(i)
                                .arg(expected)
                                .arg(static_cast<char>(buffer[static_cast<size_t>(i)]))));
    }
}

QTEST_MAIN(TestKioStream)
#include "test_kiostream.moc"
