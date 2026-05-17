/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "dragonkiostream.h"

#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>

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
    void testReadBlocksUntilData();
    void testReadCancellation();
    void testSeeking();

private:
    QString m_testFilePath;
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
}

void TestKioStream::cleanupTestCase()
{
    if (!m_testFilePath.isEmpty()) {
        QFile::remove(m_testFilePath);
    }
}

void TestKioStream::testConstruction()
{
    DragonKioStream stream;
    QVERIFY(true);
}

void TestKioStream::testSetUrl()
{
    DragonKioStream stream;
    QUrl testUrl = QUrl::fromLocalFile(m_testFilePath);
    stream.setUrl(testUrl);
    QVERIFY(true);
}

void TestKioStream::testStartStop()
{
    DragonKioStream stream;
    stream.setUrl(QUrl::fromLocalFile(m_testFilePath));
    stream.start();
    stream.stop();
    QVERIFY(true);
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

void TestKioStream::testReadBlocksUntilData()
{
    DragonKioStream stream;

    std::atomic<bool> readCompleted{false};
    std::vector<uint8_t> buffer(1024);

    {
        std::jthread readThread([&]() {
            std::stop_source stopSource;
            stream.read(buffer, stopSource.get_token());
            readCompleted = true;
        });

        QTest::qWait(100);
        QVERIFY(!readCompleted.load());

        stream.stop();
    }
    QVERIFY(readCompleted.load());
}

void TestKioStream::testReadCancellation()
{
    DragonKioStream stream;
    std::atomic<bool> readCompleted{false};
    std::vector<uint8_t> buffer(1024);

    std::stop_source stopSource;
    {
        std::jthread readThread([&]() {
            stream.read(buffer, stopSource.get_token());
            readCompleted = true;
        });

        QTest::qWait(100);
        QVERIFY(!readCompleted.load());

        stopSource.request_stop();
    }
    QVERIFY(readCompleted.load());
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

QTEST_MAIN(TestKioStream)
#include "test_kiostream.moc"
