/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "test_utils.h"

#include "player/pipe.h"
#include "sink/callbackquiesce.h"
#include "sink/nullaudiosink.h"
#include "sink/playbackpipeline.h"
#include "sink/playbackpipelinefactory.h"
#include <DragonAudioOutput>

#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// Phase-1 threading remediation tests for the audio sink backends.
//
// What is testable headlessly:
//  - the CallbackQuiesce handshake itself (unit level: concurrent
//    callback enter/exit vs. close() waiting, with and without in-flight
//    callbacks);
//  - the null sink still passing through the pipeline SPI;
//  - the SDL backend's open/pause/resume/close lifecycle through the
//    plugin factory with SDL_AUDIODRIVER=dummy (no audio hardware).
//
// What is NOT testable without a real backend, and is therefore only covered
// by code review:
//  - the SDL "store m_session before SetAudioStreamGetCallback/Resume"
//    ordering: observing it would require intercepting SDL's internal
//    callback scheduling, which the dummy driver does not expose;
//  - the PulseAudio/PipeWire quiesce, open-flag ordering, param-changed
//    dedup and post-EOS empty-buffer behavior: these need live daemons and
//    are exercised by the test_audiooutput_*/test_e2e_*_gapless suites.
class TestSinkBackends : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // CallbackQuiesce unit tests
    void testQuiesceIdleByDefault();
    void testQuiesceCallbackBailsWhileClosing();
    void testQuiesceWaitTimesOutWithCallbackInFlight();
    void testQuiesceWaitsForConcurrentCallbacks();
    void testQuiesceWakeupOnCallbackExit();

    // Backend smoke tests
    void testNullSinkStillPasses();
    void testSdlLifecycleWithDummyDriver();
};

void TestSinkBackends::testQuiesceIdleByDefault()
{
    CallbackQuiesce quiesce;

    QVERIFY(quiesce.isIdle());
    QVERIFY(!quiesce.isClosing());
    QVERIFY(quiesce.waitForIdle(std::chrono::milliseconds(100)));

    quiesce.beginClose();
    QVERIFY(quiesce.isClosing());
    QVERIFY(quiesce.waitForIdle(std::chrono::milliseconds(100)));
    QVERIFY(quiesce.isClosing());
}

void TestSinkBackends::testQuiesceCallbackBailsWhileClosing()
{
    CallbackQuiesce quiesce;

    // A callback that entered before close() started must not be told to
    // bail out (close() waits for it instead).
    {
        CallbackQuiesce::ScopedCallback before(quiesce);
        QVERIFY(!before.closing());

        quiesce.beginClose();

        // A callback entering after beginClose() must observe closing() and
        // return without touching any native object. Its guard still counts
        // as in flight until destroyed.
        CallbackQuiesce::ScopedCallback after(quiesce);
        QVERIFY(after.closing());
        QVERIFY(!quiesce.isIdle());
    }

    // Both guards destroyed: close() may proceed.
    QVERIFY(quiesce.isIdle());
    QVERIFY(quiesce.waitForIdle(std::chrono::milliseconds(100)));
}

void TestSinkBackends::testQuiesceWaitTimesOutWithCallbackInFlight()
{
    CallbackQuiesce quiesce;
    quiesce.beginClose();

    {
        CallbackQuiesce::ScopedCallback guard(quiesce);

        QElapsedTimer timer;
        timer.start();
        QVERIFY(!quiesce.waitForIdle(std::chrono::milliseconds(50)));
        // The wait must actually be bounded by the timeout, not return
        // early while the callback is still in flight.
        QVERIFY(timer.elapsed() >= 40);
        QVERIFY(!quiesce.isIdle());
    }

    // Once the in-flight callback exits, the wait succeeds promptly.
    QVERIFY(quiesce.waitForIdle(std::chrono::milliseconds(1000)));
}

void TestSinkBackends::testQuiesceWaitsForConcurrentCallbacks()
{
    CallbackQuiesce quiesce;

    constexpr int kThreadCount = 4;
    std::atomic<bool> stop{false};
    std::atomic<int> iterations{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back([&quiesce, &stop, &iterations]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // Simulated RT callback: enter, do a tiny bit of "work",
                // exit. Callbacks entering after beginClose() see
                // closing() == true and must exit without any work.
                CallbackQuiesce::ScopedCallback guard(quiesce);
                if (guard.closing()) {
                    continue;
                }
                iterations.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }

    // Let the simulated callbacks run for a moment, then race close()
    // against them: close() must observe a consistent in-flight count and
    // quiesce within the bounded wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    QVERIFY(iterations.load(std::memory_order_relaxed) > 0);

    quiesce.beginClose();
    QVERIFY(quiesce.waitForIdle(std::chrono::milliseconds(500)));

    stop.store(true, std::memory_order_relaxed);
    for (auto &t : threads) {
        t.join();
    }
}

void TestSinkBackends::testQuiesceWakeupOnCallbackExit()
{
    CallbackQuiesce quiesce;
    quiesce.beginClose();

    // One "callback" stays in flight until released. close()'s wait must be
    // woken by the callback's exit (atomic notify), not by its own timeout.
    std::atomic<bool> inFlightEntered{false};
    std::atomic<bool> releaseInFlight{false};

    std::thread callback([&]() {
        // The guard is in flight even though it observed closing(); its
        // destructor is what close() waits for.
        CallbackQuiesce::ScopedCallback guard(quiesce);
        inFlightEntered.store(true, std::memory_order_release);
        while (!releaseInFlight.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    while (!inFlightEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    QElapsedTimer timer;
    timer.start();

    std::atomic<bool> quiesced{false};
    std::thread closer([&]() {
        quiesced.store(quiesce.waitForIdle(std::chrono::milliseconds(3000)), std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    releaseInFlight.store(true, std::memory_order_release);

    callback.join();
    closer.join();

    QVERIFY(quiesced.load(std::memory_order_acquire));
    // Woken well before the 3s timeout: the callback exited ~50ms in.
    QVERIFY2(timer.elapsed() < 2500, "close() wait should be woken by the callback exit, not by its timeout");
}

void TestSinkBackends::testNullSinkStillPasses()
{
    PlaybackPipeline pipeline(std::make_unique<NullAudioSink>());
    QVERIFY(pipeline.probe());
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(!pipeline.isPaused());
    QCOMPARE(pipeline.deviceQueuedSamples(), qint64(0));

    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);
    pipeline.open(44100, 2);
    pipeline.setVolume(0.5f);
    pipeline.setMuted(true);
    pipeline.pause();
    pipeline.resume();
    pipeline.clearStream();
    QVERIFY(pipeline.close());
}

void TestSinkBackends::testSdlLifecycleWithDummyDriver()
{
    // The SDL backend with the dummy audio driver: exercises the stream
    // lifecycle (device opened by the first stream, closed by the last) and
    // the main-thread serialization of pause/resume/setGain/clearStream/
    // deviceQueuedSamples against close(), without audio hardware.
    EnvGuard guard({
        {"DRAGON_AUDIO_SINK", QByteArray("dragonsdlaudiosink")},
        {"SDL_AUDIODRIVER", QByteArray("dummy")},
    });

    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::SDL);
    QVERIFY(pipeline->probe());

    Pipe<float> pipe;
    pipeline->setAudioPipe(&pipe);
    pipeline->open(44100, 2);
    QVERIFY2(pipeline->isDeviceOpen(), "SDL dummy-driver device should open synchronously");

    // Main-thread control methods while the (dummy) device is running; with
    // the real driver these race the RT callback, which is what the
    // main-thread mutex serializes.
    pipeline->pause();
    QVERIFY(pipeline->isPaused());
    pipeline->resume();
    QVERIFY(!pipeline->isPaused());
    pipeline->setVolume(0.5f);
    pipeline->setMuted(true);
    QVERIFY(pipeline->muted());
    pipeline->setVolume(0.25f);
    pipeline->setMuted(false);
    QVERIFY(!pipeline->muted());
    QVERIFY(pipeline->deviceQueuedSamples() >= 0);
    pipeline->clearStream();

    // Reopen path: open() closes the previous stream first.
    pipeline->open(48000, 2);
    QVERIFY(pipeline->isDeviceOpen());
    QVERIFY(pipeline->hasFormat(48000, 2));

    QVERIFY(pipeline->close());
    QVERIFY(!pipeline->isDeviceOpen());
}

QTEST_MAIN(TestSinkBackends)
#include "test_sink_backends.moc"
