/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

#include "dragonpipe_test_utils.h"
#include "player/dragonpipe.h"
#include "sink/dragonaudiosink.h"
#include "sink/dragonaudiosinkfactory.h"

#include <atomic>
#include <chrono>
#include <memory>
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

    void testQueueReadyApi();
    void testFlushOpensGate();

    void testGaplessTransition();

    void testStartPaused();

    void testPauseResumeCycle();

    void testIsPausedBasic();
    void testIsPausedAfterStop();

    void testSeekWhilePaused();
    void testPositionStabilityDuringPause();

private:
    void fillQueue(DragonPipe<std::float32_t> *pipe, const std::vector<std::float32_t> &data);
};

void TestAudioOutput::testConstruction()
{
    auto output = createAudioSink();
    QVERIFY(output != nullptr);
}

void TestAudioOutput::testVolumeSetGet()
{
    auto output = createAudioSink();

    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);

    output->setVolume(0.5f);
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setVolume(0.0f);
    QVERIFY(qAbs(output->volume() - 0.0f) < 0.01f);

    output->setVolume(1.5f);
    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);

    output->setVolume(1.0f);
    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);
}

void TestAudioOutput::testMuteSetGet()
{
    auto output = createAudioSink();

    QVERIFY(!output->muted());

    output->setMuted(true);
    QVERIFY(output->muted());

    output->setMuted(false);
    QVERIFY(!output->muted());

    output->setMuted(true);
    QVERIFY(output->muted());
    output->setMuted(true);
    QVERIFY(output->muted());
}

void TestAudioOutput::testVolumeMuteInteraction()
{
    auto output = createAudioSink();

    output->setVolume(0.5f);
    output->setMuted(true);
    QVERIFY(output->muted());
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setMuted(false);
    QVERIFY(!output->muted());
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setMuted(true);
    output->setVolume(0.3f);
    QVERIFY(output->muted());
    QVERIFY(qAbs(output->volume() - 0.3f) < 0.01f);
}

void TestAudioOutput::testVolumeChangedSignal()
{
    auto output = createAudioSink();
    QSignalSpy spy(output.get(), &DragonAudioSink::volumeChanged);

    output->setVolume(0.7f);
    QVERIFY(spy.count() > 0);

    int countBefore = spy.count();
    output->setVolume(0.7f);
    QVERIFY2(spy.count() == countBefore, "Setting same volume should not emit signal");

    QSignalSpy muteSpy(output.get(), &DragonAudioSink::volumeChanged);
    output->setMuted(true);
    QVERIFY(muteSpy.count() > 0);
}

void TestAudioOutput::testSetQueue()
{
    auto output = createAudioSink();
    DragonPipe<std::float32_t> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(true);

    output->setAudioPipe(nullptr);
    QVERIFY(true);
}

void TestAudioOutput::testSetStreamName()
{
    auto output = createAudioSink();

    output->setStreamName("Test Audio"_L1);
    output->setStreamName(""_L1);
    output->setStreamName("Longer Name With Spaces"_L1);
    QVERIFY(true);
}

void TestAudioOutput::testPositionMsCalculation()
{
    auto output = createAudioSink();

    QVERIFY(output->positionMs() == 0);

    QVERIFY(output->totalSamplesWritten() == 0);
}

void TestAudioOutput::testTotalSamplesWritten()
{
    auto output = createAudioSink();

    QVERIFY(output->totalSamplesWritten() == 0);
}

void TestAudioOutput::testReset()
{
    auto output = createAudioSink();

    output->reset();
    QVERIFY(output->positionMs() == 0);
    QVERIFY(true);
}

void TestAudioOutput::testStopWithoutStart()
{
    auto output = createAudioSink();

    output->close();
    QVERIFY(true);

    output->close();
    QVERIFY(true);
}

void TestAudioOutput::testStartStopLifecycle()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    QTest::qWait(100);

    output->close();

    QVERIFY(true);

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testMultipleStartStopCycles()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 3; ++i) {
        output->open(44100, 2);
        QTest::qWait(50);
        output->close();
        output->reset();
    }

    QVERIFY(true);
}

void TestAudioOutput::testAudioDataProcessing()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    std::vector<std::float32_t> audioData(4096, 0.5f);
    fillQueue(&pipe, audioData);

    output->open(44100, 2);

    QTest::qWait(50);
    fillQueue(&pipe, std::vector<std::float32_t>(2048, 0.3f));

    QTest::qWait(100);

    int64_t samples = output->totalSamplesWritten();
    QVERIFY(samples >= 0);

    output->close();
}

void TestAudioOutput::testPositionTrackingWithData()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    std::vector<std::float32_t> oneSecond(44100, 0.5f);
    fillQueue(&pipe, oneSecond);

    QTest::qWait(1500);

    int64_t posMs = output->positionMs();
    QVERIFY(posMs >= 0);

    output->close();
    output->reset();

    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testQueueBehavior()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(true);

    auto output2 = createAudioSink();

    DragonPipe<std::float32_t> pipe2(65536);
    output2->setAudioPipe(&pipe2);
    output2->open(44100, 2);

    fillQueue(&pipe2, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(100);

    output2->close();
}

void TestAudioOutput::testStartWhileAlreadyStarted()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    output->open(48000, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(48000, 2));

    output->close();
}

void TestAudioOutput::testStopWithActiveCallbacks()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<std::float32_t>(8192, 0.5f));

    QTest::qWait(20);

    fillQueue(&pipe, std::vector<std::float32_t>(4096, 0.3f));

    output->close();
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testRapidStartStopCycles()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 10; ++i) {
        output->open(44100, 2);
        QVERIFY(output->isDeviceOpen());

        fillQueue(&pipe, std::vector<std::float32_t>(2048, 0.5f));

        QTest::qWait(5);

        fillQueue(&pipe, std::vector<std::float32_t>(2048, 0.3f));

        output->close();
        QVERIFY(!output->isDeviceOpen());
        output->reset();
    }

    QVERIFY(true);
}

void TestAudioOutput::testStopDuringStarvation()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    output->close();
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testQueueReadyApi()
{
    auto output = createAudioSink();

    QVERIFY(output->isQueueReady());

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    output->setQueueReady(true);
    QVERIFY(output->isQueueReady());

    output->setQueueReady(true);
    QVERIFY(output->isQueueReady());
    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());
    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());
}

void TestAudioOutput::testFlushOpensGate()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(50);

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    output->setPositionOffset(0, DragonAudioSink::PositionResetMode::NormalTrackChange);

    QTest::qWait(100);

    QVERIFY2(output->isQueueReady(), "SDL callback should have processed flush and opened the gate");

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testGaplessTransition()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<std::float32_t>(65536, 0.5f));
    QTest::qWait(300);

    const int64_t samplesBefore = output->totalSamplesWritten();
    QVERIFY2(samplesBefore > 10000, "Counter should have grown large after 300ms of playback");
    const size_t queueSizeBefore = pipe.consumer().ready();
    QVERIFY2(queueSizeBefore > 0, "Queue should still have items");

    output->setPositionOffset(0, DragonAudioSink::PositionResetMode::GaplessTransition);

    QTest::qWait(100);

    const size_t queueSizeAfter = pipe.consumer().ready();
    QVERIFY2(queueSizeAfter > 0, "Queue should not be drained during GaplessTransition");

    output->close();
    QVERIFY(!output->isDeviceOpen());

    const int64_t samplesAfter = output->totalSamplesWritten();
    QVERIFY2(samplesAfter < 20000, "totalSamplesWritten should be small after GaplessTransition reset");
}

void TestAudioOutput::testStartPaused()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();

    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    QCOMPARE(output->positionMs(), 0);
    QCOMPARE(output->totalSamplesWritten(), 0);

    output->resume();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testPauseResumeCycle()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(100);

    const int64_t posBefore = output->positionMs();
    const int64_t writtenBefore = output->totalSamplesWritten();

    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->isPaused());

    QTest::qWait(200);

    QCOMPARE(output->positionMs(), posBefore);

    const int64_t writtenAfterPause = output->totalSamplesWritten();
    QVERIFY2(writtenAfterPause <= writtenBefore + 2048, "Pause should stop or significantly reduce sample consumption");

    QTest::qWait(200);
    QCOMPARE(output->totalSamplesWritten(), writtenAfterPause);

    output->resume();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testIsPausedBasic()
{
    auto output = createAudioSink();

    QVERIFY(!output->isPaused());

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isPaused());

    output->resume();
    QVERIFY(!output->isPaused());

    output->close();
}

void TestAudioOutput::testIsPausedAfterStop()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();
    QVERIFY(output->isPaused());

    output->close();
    QVERIFY(!output->isPaused());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testSeekWhilePaused()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(100);

    output->pause();
    QVERIFY(output->isPaused());

    QVERIFY(pipe.consumer().ready() == 0);
    fillQueue(&pipe, std::vector<std::float32_t>(8192, 0.5f));
    const size_t queueSizeBeforeSeek = pipe.consumer().ready();
    QVERIFY2(queueSizeBeforeSeek > 0, "Queue should have stale samples before seek");

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    const int64_t seekTargetMs = 30000;
    output->setPositionOffset(seekTargetMs, DragonAudioSink::PositionResetMode::Seek);

    const int64_t posAfterSeek = output->positionMs();
    QCOMPARE(posAfterSeek, seekTargetMs);

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    QVERIFY(output->isQueueReady());

    output->resume();
    QVERIFY(!output->isPaused());

    output->close();
}

void TestAudioOutput::testPositionStabilityDuringPause()
{
    auto output = createAudioSink();

    DragonPipe<std::float32_t> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    fillQueue(&pipe, std::vector<std::float32_t>(4096, 0.5f));
    QTest::qWait(100);

    output->pause();
    const int64_t posAtPause = output->positionMs();

    for (int i = 0; i < 5; ++i) {
        QTest::qWait(100);
        const int64_t currentPos = output->positionMs();
        QCOMPARE(currentPos, posAtPause);
    }

    output->resume();
    QTest::qWait(100);

    const int64_t posAfterResume = output->positionMs();
    QVERIFY(posAfterResume >= posAtPause);

    output->close();
}

void TestAudioOutput::fillQueue(DragonPipe<std::float32_t> *pipe, const std::vector<std::float32_t> &data)
{
    QVERIFY(pipe != nullptr);
    pipe->producer().write(data, std::stop_token{});
}

QTEST_MAIN(TestAudioOutput)
#include "test_audiooutput.moc"
