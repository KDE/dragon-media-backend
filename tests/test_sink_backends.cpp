/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "test_utils.h"

#include "player/pipe.h"
#include "sink/callbackquiesce.h"
#include "sink/playbackpipeline.h"
#include "sink/playbackpipelinefactory.h"
#include "sink/pulsesilencefill.h"
#include <DragonAudioOutput>

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

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
    void testSdlLifecycleWithDummyDriver();

    void testPulseSilenceFillFrameAlignment();
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

void TestSinkBackends::testPulseSilenceFillFrameAlignment()
{
    struct FakeStream {
        std::size_t chunkBytes;
        bool failBegin = false;
        bool failWrite = false;
        std::size_t beginCalls = 0;
        std::size_t cancelCalls = 0;
        std::vector<std::size_t> writeSizes;
        std::vector<std::uint8_t> written;
        std::vector<std::uint8_t> arena;

        explicit FakeStream(std::size_t chunk)
            : chunkBytes(chunk)
            , arena(chunk)
        {
            std::fill(arena.begin(), arena.end(), std::uint8_t{0xAB});
        }

        int begin_write(void **buffer, std::size_t *bytes)
        {
            ++beginCalls;
            if (failBegin) {
                return -1;
            }
            *buffer = arena.data();
            *bytes = chunkBytes;
            return 0;
        }

        int write(void *buffer, std::size_t bytes)
        {
            if (failWrite) {
                return -1;
            }
            const auto *begin = static_cast<const std::uint8_t *>(buffer);
            written.insert(written.end(), begin, begin + bytes);
            writeSizes.push_back(bytes);
            return 0;
        }

        void cancel_write()
        {
            ++cancelCalls;
        }

        [[nodiscard]] bool allZero() const
        {
            return std::all_of(written.begin(), written.end(), [](std::uint8_t byte) {
                return byte == 0;
            });
        }
    };

    const auto fill = [](FakeStream &stream, std::size_t bytes, int channels) {
        return fillStreamWithSilence(
            bytes,
            channels,
            [&stream](void **buffer, std::size_t *chunk) {
                return stream.begin_write(buffer, chunk);
            },
            [&stream](void *buffer, std::size_t chunk) {
                return stream.write(buffer, chunk);
            },
            [&stream]() {
                stream.cancel_write();
            });
    };

    {
        FakeStream stream(65536);
        QCOMPARE(fill(stream, 8192, 2), std::size_t{8192});
        QCOMPARE(stream.writeSizes, std::vector<std::size_t>{8192});
        QVERIFY(stream.allZero());
        QCOMPARE(stream.cancelCalls, std::size_t{0});
    }

    {
        FakeStream stream(1001);
        QCOMPARE(fill(stream, 4096, 2), std::size_t{4096});
        QCOMPARE(stream.beginCalls, std::size_t{5});
        QCOMPARE(stream.writeSizes, std::vector<std::size_t>({1000, 1000, 1000, 1000, 96}));
        QVERIFY(stream.allZero());
    }

    {
        FakeStream stream(65536);
        QCOMPARE(fill(stream, 4097, 5), std::size_t{4080});
        QCOMPARE(stream.writeSizes, std::vector<std::size_t>{4080});
        QVERIFY(stream.allZero());
    }

    {
        FakeStream stream(65536);
        QCOMPARE(fill(stream, 6, 2), std::size_t{0});
        QCOMPARE(stream.beginCalls, std::size_t{0});
    }

    {
        FakeStream stream(4);
        QCOMPARE(fill(stream, 8192, 2), std::size_t{0});
        QCOMPARE(stream.cancelCalls, std::size_t{1});
        QVERIFY(stream.writeSizes.empty());
    }

    {
        FakeStream stream(65536);
        stream.failBegin = true;
        QCOMPARE(fill(stream, 8192, 2), std::size_t{0});
        QCOMPARE(stream.beginCalls, std::size_t{1});
    }
    {
        FakeStream stream(1000);
        stream.failWrite = true;
        QCOMPARE(fill(stream, 4096, 2), std::size_t{0});
        QCOMPARE(stream.beginCalls, std::size_t{1});
    }
}

QTEST_MAIN(TestSinkBackends)
#include "test_sink_backends.moc"
