/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "stream/bufferprogress.h"
#include "stream/stream.h"
#include "stream/streambuffer.h"

#include <array>
#include <atomic>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

// Dummy stream to test signal forwarding from StreamBuffer
class MockStream : public Stream
{
    Q_OBJECT
public:
    explicit MockStream(QObject *parent = nullptr)
        : Stream(parent)
    {
    }

    void setUrl(const QUrl &) override
    {
    }
    void start() override
    {
    }
    void stop() override
    {
    }
    int read(std::span<uint8_t>, const std::stop_token &) override
    {
        return 0;
    }
    BufferProgress *bufferProgress() const override
    {
        return nullptr;
    }
};

class TestStreamBuffer : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testInitialState();
    void testBasicReadWrite();
    void testMultipleWritesSingleRead();
    void testSingleWriteMultipleReads();
    void testEmptySpan();
    void testReadAfterFinishWithRemainingData();
    void testWatermarkTransitions();
    void testBackpressureDetection();
    void testBlockingReadUnblockedByWrite();
    void testBlockingReadUnblockedByAbort();
    void testBlockingReadUnblockedByError();
    void testBlockingReadUnblockedByFinish();
    void testStopTokenCancellation();
    void testReset();
    void testPositionTracking();
    void testTotalSize();
    void testBufferProgress();
    void testStreamSignalIntegration();
};

void TestStreamBuffer::testInitialState()
{
    StreamBuffer buffer;
    QCOMPARE(buffer.depth(), 0);
    QCOMPARE(buffer.position(), 0);
    QCOMPARE(buffer.totalSize(), -1);
    QVERIFY(buffer.isEmpty());
    QVERIFY(!buffer.isFull());
    QVERIFY(!buffer.isBuffering());
    QVERIFY(!buffer.isAborted());
    QVERIFY(!buffer.hasError());
    QVERIFY(!buffer.isFinished());
    QVERIFY(buffer.bufferProgress() != nullptr);
}

void TestStreamBuffer::testBasicReadWrite()
{
    StreamBuffer buffer;
    const QByteArray data = "Hello, world!";

    const auto writeRes = buffer.write(data);
    QCOMPARE(writeRes.newDepth, data.size());
    QCOMPARE(buffer.depth(), data.size());
    QVERIFY(!buffer.isEmpty());

    std::stop_source ss;
    std::array<uint8_t, 64> dst{};
    const auto readRes = buffer.readDetailed(dst, ss.get_token());

    QCOMPARE(readRes.bytesRead, data.size());
    QCOMPARE(buffer.depth(), 0);
    QCOMPARE(buffer.position(), data.size());
    QVERIFY(buffer.isEmpty());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(dst.data()), readRes.bytesRead), data);
}

void TestStreamBuffer::testMultipleWritesSingleRead()
{
    StreamBuffer buffer;
    const QByteArray chunk1 = "Chunk One; ";
    const QByteArray chunk2 = "Chunk Two; ";
    const QByteArray chunk3 = "Chunk Three.";
    const QByteArray expected = chunk1 + chunk2 + chunk3;

    buffer.write(chunk1);
    buffer.write(chunk2);
    buffer.write(chunk3);
    QCOMPARE(buffer.depth(), expected.size());

    std::stop_source ss;
    std::vector<uint8_t> dst(expected.size() * 2);
    const int bytesRead = buffer.read(dst, ss.get_token());

    QCOMPARE(bytesRead, expected.size());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(dst.data()), bytesRead), expected);
    QCOMPARE(buffer.depth(), 0);
    QCOMPARE(buffer.position(), expected.size());
}

void TestStreamBuffer::testSingleWriteMultipleReads()
{
    StreamBuffer buffer;
    const QByteArray data = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    buffer.write(data);

    std::stop_source ss;
    QByteArray combined;
    std::array<uint8_t, 10> dst{};

    while (buffer.depth() > 0) {
        const int bytesRead = buffer.read(dst, ss.get_token());
        QVERIFY(bytesRead > 0);
        combined.append(reinterpret_cast<const char *>(dst.data()), bytesRead);
    }

    QCOMPARE(combined, data);
    QCOMPARE(buffer.depth(), 0);
    QCOMPARE(buffer.position(), data.size());
}

void TestStreamBuffer::testEmptySpan()
{
    StreamBuffer buffer;
    const auto emptyRes = buffer.write(QByteArray{});
    QCOMPARE(emptyRes.newDepth, 0);
    QVERIFY(!emptyRes.exitedBuffering);
    QVERIFY(!emptyRes.isFull);

    const auto emptyViewRes = buffer.write(QByteArrayView{});
    QCOMPARE(emptyViewRes.newDepth, 0);

    buffer.write(QByteArray("test"));

    std::stop_source ss;
    std::span<uint8_t> emptySpan{};
    const int bytesRead = buffer.read(emptySpan, ss.get_token());

    QCOMPARE(bytesRead, 0);
    QCOMPARE(buffer.depth(), 4);
    QCOMPARE(buffer.position(), 0);
}

void TestStreamBuffer::testReadAfterFinishWithRemainingData()
{
    StreamBuffer buffer;
    buffer.write(QByteArray("Hello"));
    buffer.setFinished();
    QVERIFY(buffer.isFinished());

    std::stop_source ss;
    std::array<uint8_t, 64> dst{};
    int n = buffer.read(dst, ss.get_token());
    QCOMPARE(n, 5);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(dst.data()), n), QByteArray("Hello"));

    n = buffer.read(dst, ss.get_token());
    QCOMPARE(n, 0);
}

void TestStreamBuffer::testWatermarkTransitions()
{
    StreamBuffer buffer(nullptr,
                        StreamBufferOptions{
                            .lowWaterMark = 0,
                            .highWaterMark = 100,
                            .maxBufferBytes = 1000,
                        });

    auto wRes = buffer.write(QByteArray(50, 'a'));
    QCOMPARE(wRes.newDepth, 50);
    QVERIFY(!wRes.exitedBuffering);

    // Read 50 bytes -> depth drops to 0 (<= lowWaterMark) -> enters buffering
    std::stop_source ss;
    std::vector<uint8_t> dst(50);
    auto rRes = buffer.readDetailed(dst, ss.get_token());
    QCOMPARE(rRes.bytesRead, 50);
    QVERIFY(rRes.enteredBuffering);
    QVERIFY(buffer.isBuffering());

    // Write 40 bytes -> depth = 40 (< 100) -> does NOT exit buffering
    wRes = buffer.write(QByteArray(40, 'b'));
    QCOMPARE(wRes.newDepth, 40);
    QVERIFY(!wRes.exitedBuffering);
    QVERIFY(buffer.isBuffering());

    // Write 60 bytes -> depth = 100 (>= highWaterMark) -> exits buffering
    wRes = buffer.write(QByteArray(60, 'c'));
    QCOMPARE(wRes.newDepth, 100);
    QVERIFY(wRes.exitedBuffering);
    QVERIFY(!buffer.isBuffering());

    // Another write while already above highWaterMark should not trigger exitedBuffering again
    wRes = buffer.write(QByteArray(20, 'd'));
    QVERIFY(!wRes.exitedBuffering);
}

void TestStreamBuffer::testBackpressureDetection()
{
    StreamBuffer buffer(nullptr,
                        StreamBufferOptions{
                            .lowWaterMark = 0,
                            .highWaterMark = 100,
                            .maxBufferBytes = 500,
                        });

    std::atomic<bool> backpressureRelievedCalled{false};
    buffer.setBackpressureRelievedCallback([&]() {
        backpressureRelievedCalled = true;
    });

    auto wRes = buffer.write(QByteArray(450, 'x'));
    QVERIFY(!wRes.isFull);
    QVERIFY(!buffer.isFull());

    // Write 100 bytes -> depth = 550 >= 500 -> isFull!
    wRes = buffer.write(QByteArray(100, 'y'));
    QVERIFY(wRes.isFull);
    QVERIFY(buffer.isFull());

    // Read 20 bytes -> depth = 530, still >= 500 -> backpressureRelieved = false
    std::stop_source ss;
    std::vector<uint8_t> dst(20);
    auto rRes = buffer.readDetailed(dst, ss.get_token());
    QCOMPARE(rRes.bytesRead, 20);
    QVERIFY(!rRes.backpressureRelieved);
    QVERIFY(!backpressureRelievedCalled.load());
    QVERIFY(buffer.isFull());

    // Read 40 bytes -> depth = 490 < 500 -> backpressureRelieved = true!
    dst.resize(40);
    rRes = buffer.readDetailed(dst, ss.get_token());
    QCOMPARE(rRes.bytesRead, 40);
    QVERIFY(rRes.backpressureRelieved);
    QVERIFY(backpressureRelievedCalled.load());
    QVERIFY(!buffer.isFull());
}

void TestStreamBuffer::testBlockingReadUnblockedByWrite()
{
    StreamBuffer buffer;
    std::stop_source ss;
    std::atomic<int> readBytes{0};
    const QByteArray payload = "payload data";

    std::thread reader([&]() {
        std::array<uint8_t, 64> dst{};
        readBytes = buffer.read(dst, ss.get_token());
    });

    // Ensure reader thread has started and blocked
    QTest::qWait(50);
    QCOMPARE(readBytes.load(), 0);

    buffer.write(payload);
    reader.join();

    QCOMPARE(readBytes.load(), payload.size());
}

void TestStreamBuffer::testBlockingReadUnblockedByAbort()
{
    StreamBuffer buffer;
    std::stop_source ss;
    std::atomic<int> readResult{0};

    std::thread reader([&]() {
        std::array<uint8_t, 64> dst{};
        readResult = buffer.read(dst, ss.get_token());
    });

    QTest::qWait(50);
    buffer.abort();
    reader.join();

    QCOMPARE(readResult.load(), -1);
    QVERIFY(buffer.isAborted());
}

void TestStreamBuffer::testBlockingReadUnblockedByError()
{
    StreamBuffer buffer;
    std::stop_source ss;
    std::atomic<int> readResult{0};

    std::thread reader([&]() {
        std::array<uint8_t, 64> dst{};
        readResult = buffer.read(dst, ss.get_token());
    });

    QTest::qWait(50);
    buffer.setError();
    reader.join();

    QCOMPARE(readResult.load(), -1);
    QVERIFY(buffer.hasError());
}

void TestStreamBuffer::testBlockingReadUnblockedByFinish()
{
    StreamBuffer buffer;
    std::stop_source ss;
    std::atomic<int> readResult{-99};

    std::thread reader([&]() {
        std::array<uint8_t, 64> dst{};
        readResult = buffer.read(dst, ss.get_token());
    });

    QTest::qWait(50);
    buffer.setFinished();
    reader.join();

    QCOMPARE(readResult.load(), 0);
    QVERIFY(buffer.isFinished());
}

void TestStreamBuffer::testStopTokenCancellation()
{
    StreamBuffer buffer;
    std::stop_source ss;
    std::atomic<int> readResult{0};

    std::thread reader([&]() {
        std::array<uint8_t, 64> dst{};
        readResult = buffer.read(dst, ss.get_token());
    });

    QTest::qWait(50);
    ss.request_stop();
    reader.join();

    QCOMPARE(readResult.load(), -1);
}

void TestStreamBuffer::testReset()
{
    StreamBuffer buffer;
    buffer.write(QByteArray("data"));
    buffer.abort();
    QVERIFY(buffer.isAborted());

    buffer.reset(1234);
    QVERIFY(!buffer.isAborted());
    QVERIFY(!buffer.hasError());
    QVERIFY(!buffer.isFinished());
    QVERIFY(!buffer.isBuffering());
    QCOMPARE(buffer.depth(), 0);
    QCOMPARE(buffer.position(), 1234);
    QVERIFY(buffer.isEmpty());
}

void TestStreamBuffer::testPositionTracking()
{
    StreamBuffer buffer;
    QCOMPARE(buffer.position(), 0);

    buffer.setPosition(100);
    QCOMPARE(buffer.position(), 100);

    buffer.write(QByteArray(50, 'z'));
    std::stop_source ss;
    std::vector<uint8_t> dst(20);
    int n = buffer.read(dst, ss.get_token());
    QCOMPARE(n, 20);
    QCOMPARE(buffer.position(), 120);

    buffer.reset(500);
    QCOMPARE(buffer.position(), 500);
}

void TestStreamBuffer::testTotalSize()
{
    StreamBuffer buffer;
    QCOMPARE(buffer.totalSize(), -1);

    buffer.setTotalSize(1048576);
    QCOMPARE(buffer.totalSize(), 1048576);
}

void TestStreamBuffer::testBufferProgress()
{
    StreamBuffer buffer;
    QVERIFY(buffer.bufferProgress() != nullptr);

    buffer.bufferProgress()->setBytesReceived(2048);
    buffer.reset();
    QCOMPARE(buffer.bufferProgress()->progress(), 0.0);
}

void TestStreamBuffer::testStreamSignalIntegration()
{
    MockStream stream;
    StreamBuffer buffer(&stream,
                        StreamBufferOptions{
                            .lowWaterMark = 0,
                            .highWaterMark = 100,
                            .maxBufferBytes = 500,
                        });

    QSignalSpy bufferingSpy(&stream, &Stream::streamBuffering);
    QSignalSpy bufferedSpy(&stream, &Stream::streamBuffered);
    QSignalSpy errorSpy(&stream, &Stream::errorOccurred);

    buffer.write(QByteArray(50, 'x'));
    QCOMPARE(bufferedSpy.count(), 0);

    // Read to 0 -> enters buffering -> emits streamBuffering
    std::stop_source ss;
    std::vector<uint8_t> dst(50);
    buffer.read(dst, ss.get_token());
    QCOMPARE(bufferingSpy.count(), 1);

    // Write to 100 -> exits buffering -> emits streamBuffered
    buffer.write(QByteArray(100, 'y'));
    QCOMPARE(bufferedSpy.count(), 1);

    buffer.setError(QStringLiteral("Connection reset"));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Connection reset"));
}

QTEST_GUILESS_MAIN(TestStreamBuffer)
#include "test_streambuffer.moc"
