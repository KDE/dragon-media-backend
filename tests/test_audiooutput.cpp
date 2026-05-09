/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

#include <LockFreeSpscQueue.h>
#include <dragonaudiooutput.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

class TestAudioOutput : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConstruction();
    void testVolumeSetGet();
    void testMuteSetGet();
    void testVolumeMuteInteraction();
    void testVolumeChangedSignal();
    void testSetQueue();
    void testSetStreamName();

    void testPositionMsCalculation();
    void testTotalSamplesWritten();

    void testReset();
    void testStopWithoutStart();
    void testStartStopLifecycle();
    void testMultipleStartStopCycles();
    void testAudioDataProcessing();
    void testPositionTrackingWithData();
    void testQueueBehavior();
    void testStartWhileAlreadyStarted();

    void testStopWithActiveCallbacks();
    void testRapidStartStopCycles();
    void testStopDuringStarvation();

    void testStartPaused();

private:
    void fillQueue(LockFreeSpscQueue<std::float32_t> *queue, const std::vector<std::float32_t> &data);
};

void TestAudioOutput::testConstruction()
{
    DragonAudioOutput output;
    QVERIFY(true);
}

void TestAudioOutput::testVolumeSetGet()
{
    DragonAudioOutput output;

    QVERIFY(qAbs(output.volume() - 1.0f) < 0.01f);

    output.setVolume(0.5f);
    QVERIFY(qAbs(output.volume() - 0.5f) < 0.01f);

    output.setVolume(0.0f);
    QVERIFY(qAbs(output.volume() - 0.0f) < 0.01f);

    output.setVolume(1.5f);
    QVERIFY(qAbs(output.volume() - 1.5f) < 0.01f);

    output.setVolume(1.0f);
    QVERIFY(qAbs(output.volume() - 1.0f) < 0.01f);
}

void TestAudioOutput::testMuteSetGet()
{
    DragonAudioOutput output;

    QVERIFY(!output.muted());

    output.setMuted(true);
    QVERIFY(output.muted());

    output.setMuted(false);
    QVERIFY(!output.muted());

    output.setMuted(true);
    QVERIFY(output.muted());
    output.setMuted(true);
    QVERIFY(output.muted());
}

void TestAudioOutput::testVolumeMuteInteraction()
{
    DragonAudioOutput output;

    output.setVolume(0.5f);
    output.setMuted(true);
    QVERIFY(output.muted());
    QVERIFY(qAbs(output.volume() - 0.5f) < 0.01f);

    output.setMuted(false);
    QVERIFY(!output.muted());
    QVERIFY(qAbs(output.volume() - 0.5f) < 0.01f);

    output.setMuted(true);
    output.setVolume(0.3f);
    QVERIFY(output.muted());
    QVERIFY(qAbs(output.volume() - 0.3f) < 0.01f);
}

void TestAudioOutput::testVolumeChangedSignal()
{
    DragonAudioOutput output;
    QSignalSpy spy(&output, &DragonAudioOutput::volumeChanged);

    output.setVolume(0.7f);
    QVERIFY(spy.count() > 0);

    int countBefore = spy.count();
    output.setVolume(0.7f);
    QVERIFY2(spy.count() == countBefore, "Setting same volume should not emit signal");

    QSignalSpy muteSpy(&output, &DragonAudioOutput::volumeChanged);
    output.setMuted(true);
    QVERIFY(muteSpy.count() > 0);
}

void TestAudioOutput::testSetQueue()
{
    DragonAudioOutput output;
    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    output.setQueue(&queue);
    QVERIFY(true);

    output.setQueue(nullptr);
    QVERIFY(true);
}

void TestAudioOutput::testSetStreamName()
{
    DragonAudioOutput output;

    output.setStreamName("Test Audio"_L1);
    output.setStreamName(""_L1);
    output.setStreamName("Longer Name With Spaces"_L1);
    QVERIFY(true);
}

void TestAudioOutput::testPositionMsCalculation()
{
    DragonAudioOutput output;

    QVERIFY(output.positionMs() == 0);

    QVERIFY(output.totalSamplesWritten() == 0);
}

void TestAudioOutput::testTotalSamplesWritten()
{
    DragonAudioOutput output;

    QVERIFY(output.totalSamplesWritten() == 0);
}

void TestAudioOutput::testReset()
{
    DragonAudioOutput output;

    output.reset();
    QVERIFY(output.positionMs() == 0);
    QVERIFY(true);
}

void TestAudioOutput::testStopWithoutStart()
{
    DragonAudioOutput output;

    output.stop();
    QVERIFY(true);

    output.stop();
    QVERIFY(true);
}

void TestAudioOutput::testStartStopLifecycle()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2);

    QTest::qWait(100);

    output.stop();

    QVERIFY(true);

    output.reset();
    QVERIFY(output.positionMs() == 0);
}

void TestAudioOutput::testMultipleStartStopCycles()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    for (int i = 0; i < 3; ++i) {
        output.start(44100, 2);
        QTest::qWait(50);
        output.stop();
        output.reset();
    }

    QVERIFY(true);
}

void TestAudioOutput::testAudioDataProcessing()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    std::vector<std::float32_t> audioData(4096, 0.5f);
    fillQueue(&queue, audioData);

    output.start(44100, 2);

    QTest::qWait(50);
    fillQueue(&queue, std::vector<std::float32_t>(2048, 0.3f));

    QTest::qWait(100);

    int64_t samples = output.totalSamplesWritten();
    QVERIFY(samples >= 0);

    output.stop();
}

void TestAudioOutput::testPositionTrackingWithData()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2);

    std::vector<std::float32_t> oneSecond(44100, 0.5f);
    fillQueue(&queue, oneSecond);

    QTest::qWait(1500);

    int64_t posMs = output.positionMs();
    QVERIFY(posMs >= 0);

    output.stop();
    output.reset();

    QVERIFY(output.positionMs() == 0);
}

void TestAudioOutput::testQueueBehavior()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    output.setQueue(&queue);
    QVERIFY(true);

    DragonAudioOutput output2;

    std::vector<std::float32_t> buffer2(65536);
    LockFreeSpscQueue<std::float32_t> queue2{std::span{buffer2}};
    output2.setQueue(&queue2);
    output2.start(44100, 2);

    fillQueue(&queue2, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(100);

    output2.stop();
}

void TestAudioOutput::testStartWhileAlreadyStarted()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2);
    QVERIFY(output.isDeviceOpen());

    output.start(48000, 2);
    QVERIFY(output.isDeviceOpen());
    QVERIFY(output.hasFormat(48000, 2));

    output.stop();
}

void TestAudioOutput::testStopWithActiveCallbacks()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2);
    QVERIFY(output.isDeviceOpen());

    fillQueue(&queue, std::vector<std::float32_t>(8192, 0.5f));

    QTest::qWait(20);

    fillQueue(&queue, std::vector<std::float32_t>(4096, 0.3f));

    output.stop();
    QVERIFY(!output.isDeviceOpen());

    output.reset();
    QVERIFY(output.positionMs() == 0);
}

void TestAudioOutput::testRapidStartStopCycles()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    for (int i = 0; i < 10; ++i) {
        output.start(44100, 2);
        QVERIFY(output.isDeviceOpen());

        fillQueue(&queue, std::vector<std::float32_t>(2048, 0.5f));

        QTest::qWait(5);

        fillQueue(&queue, std::vector<std::float32_t>(2048, 0.3f));

        output.stop();
        QVERIFY(!output.isDeviceOpen());
        output.reset();
    }

    QVERIFY(true);
}

void TestAudioOutput::testStopDuringStarvation()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2);
    QVERIFY(output.isDeviceOpen());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    output.stop();
    QVERIFY(!output.isDeviceOpen());

    output.reset();
    QVERIFY(output.positionMs() == 0);
}

void TestAudioOutput::testStartPaused()
{
    DragonAudioOutput output;

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    output.setQueue(&queue);

    output.start(44100, 2, true);

    QVERIFY(output.isDeviceOpen());
    QVERIFY(output.hasFormat(44100, 2));

    QCOMPARE(output.positionMs(), 0);
    QCOMPARE(output.totalSamplesWritten(), 0);

    output.resume();
    QVERIFY(output.isDeviceOpen());
    QVERIFY(output.hasFormat(44100, 2));

    output.stop();
    QVERIFY(!output.isDeviceOpen());
}

void TestAudioOutput::fillQueue(LockFreeSpscQueue<std::float32_t> *queue, const std::vector<std::float32_t> &data)
{
    if (!queue || data.empty())
        return;

    [[maybe_unused]] const auto written = queue->try_write(data.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < data.size())
                v = data[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < data.size())
                v = data[i++];
        }
    });
}

QTEST_MAIN(TestAudioOutput)
#include "test_audiooutput.moc"