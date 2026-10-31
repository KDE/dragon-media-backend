/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "mixer/mixer.h"
#include "player/pipe.h"
#include "sink/playbackpipeline.h"
#include "sink/streamclock.h"

#include <chrono>
#include <span>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

// The mixing engine and its stream clock are plain classes, so the crossfade
// logic can be driven deterministically on the test thread, without any
// audio backend.
struct TestMixer {
    StreamClock clock;
    Mixer mixer{clock};

    void configure(int sampleRate, int channels, size_t maxSamples)
    {
        clock.setFormat(sampleRate, channels);
        mixer.preAllocate(maxSamples);
    }

    std::span<const float> callback(size_t maxSamples, std::chrono::microseconds pts = {})
    {
        return mixer.render(maxSamples, pts);
    }
};

// Flat audio makes the gain math directly observable in the captured output.
std::vector<float> makeConstant(float amplitude, int frames, int channels)
{
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(frames * channels));
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            samples.push_back(amplitude);
        }
    }
    return samples;
}

// One pipe's worth of flat audio: under the 65536-sample pipe capacity so
// test producers never block on a full ring buffer.
constexpr int kTestFrames = 32000;
constexpr int kTestChannels = 2;

void writeAll(Pipe<float> &pipe, const std::vector<float> &samples)
{
    std::stop_source source;
    const size_t written = pipe.producer().write(samples, source.get_token());
    QCOMPARE(written, samples.size());
}

std::vector<float> drainSpan(std::span<const float> span)
{
    return {span.begin(), span.end()};
}

}

class TestSinkCrossfade : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLatchHasZeroSwapWindowDropout();
    void testFadeInOnly();
    void testTransientStarvationVsRetirement();
    void testProducerDoneRetiresOnEmpty();
    void testSlowLoadEnvelopeHold();
    void testRapidRetriggerSupersedeRamp();
    void testFlushAbortsStagedCrossfadeCarriesPrimaryPipe();
    void testProcessFlushResetsFade();
    void testFadeInOnlyWithDeferredFlush();
    void testRapidRetrigger2SlotHandoff();
    void testFlushAbortsStagedCrossfadeWithExplicitPipe();
    void testSeekGatingNearEofPreservesEos();
    void testFadeInFromClosedPreservedAcrossFormatReopen();
    void testCrossfadeSupersedesPendingFlushSingleBeginStream();
};

void TestSinkCrossfade::testLatchHasZeroSwapWindowDropout()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));

    // Fade-out only (no fade-in, no slow-load hold): the very first callback
    // after arming must already produce the outgoing stream's fade-out
    // samples (no one-quantum dropout, no false underrun) even though the
    // new primary pipe is still empty.
    QVERIFY(sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 0ms).has_value());
    QVERIFY(sink.mixer.isCrossfadePending());
    QVERIFY(sink.mixer.isCrossfading());

    const auto out = sink.callback(2048);
    QVERIFY(!out.empty());
    QCOMPARE(sink.clock.underrunCount(), 0);
    QVERIFY(!sink.mixer.isCrossfadePending());

    // Frame 0 of the fade-out is at full gain (cos(0) = 1).
    const std::vector<float> captured = drainSpan(out);
    QVERIFY(qAbs(captured[0] - 0.5f) < 1e-6f);
    // The gain must be falling monotonically over the chunk.
    QVERIFY(captured[captured.size() - 2] < captured[0]);
}

void TestSinkCrossfade::testFadeInOnly()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeB;
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    QVERIFY(sink.mixer.armCrossfade(nullptr, &pipeB, 0ms, 1000ms).has_value());
    QVERIFY(sink.mixer.isCrossfadePending());

    const auto out = sink.callback(2048);
    const std::vector<float> captured = drainSpan(out);
    QCOMPARE(captured.size(), size_t{2048});
    // First frame of the fade-in starts at silence (sin(0) = 0).
    QVERIFY(qAbs(captured[0]) < 1e-6f);
    QVERIFY(qAbs(captured[captured.size() - 1]) > 1e-3f);
    QCOMPARE(sink.clock.underrunCount(), 0);
}

void TestSinkCrossfade::testTransientStarvationVsRetirement()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(!sink.callback(2048).empty());

    // The retiring producer has NOT finished: even once its pipe runs dry the
    // pipe must stay attached and resume mixing once refilled.
    while (pipeA.consumer().ready() > 0) {
        QVERIFY(!sink.callback(2048).empty());
    }
    QVERIFY(sink.mixer.isCrossfading());

    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    QVERIFY(!sink.callback(2048).empty());

    // Once the producer is done and the pipe is empty, the pipe retires.
    pipeA.setProducerDone(true);
    while (pipeA.consumer().ready() > 0) {
        QVERIFY(!sink.callback(2048).empty());
    }
    sink.callback(2048); // retires the empty, producer-done fading pipe
    QVERIFY(!sink.mixer.isCrossfading());
}

void TestSinkCrossfade::testProducerDoneRetiresOnEmpty()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, 2048, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    // The fade-out lasts 1000 ms but pipeA only holds ~46 ms: once it drains
    // and the producer is done, the pipe must retire without waiting for the
    // full fade window.
    pipeA.setProducerDone(true);
    for (int i = 0; i < 600 && sink.mixer.isCrossfading(); ++i) {
        QVERIFY(!sink.callback(2048).empty());
    }
    QVERIFY(!sink.mixer.isCrossfading());
}

void TestSinkCrossfade::testSlowLoadEnvelopeHold()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB; // incoming track: not yet producing
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);

    // While pipeB is empty the outgoing gain must hold instead of ramping to
    // silence before the incoming track ever produced a sample.
    const auto held = drainSpan(sink.callback(2048));
    QVERIFY(qAbs(held[0] - 0.5f) < 1e-6f);

    // Feed the incoming track: both envelopes ramp together now.
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    // The fade-out gain must not have collapsed while the load stalled.
    QVERIFY(out[0] > 0.4f);
}

void TestSinkCrossfade::testRapidRetriggerSupersedeRamp()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<float> pipeC;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeC, makeConstant(0.5f, kTestFrames, kTestChannels));

    // A -> B crossfade, run part-way so B is partially faded in.
    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    for (int i = 0; i < 10; ++i) {
        QVERIFY(!sink.callback(2048).empty());
    }

    // Mid-fade track change to C: stageFlush retires and drains pipeA on the
    // audio thread, ramps down pipeB via FlushTail, and installs pipeC at 100% volume.
    std::ignore = sink.mixer.stageFlush(&pipeC, 0ms);

    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    QVERIFY(!sink.mixer.isCrossfading());
    QCOMPARE(pipeA.consumer().ready(), size_t{0});
    // Micro-fade smoothly eases the previous signal into pipeC, which plays at 100% volume
    QCOMPARE(out[out.size() - 1], 0.5f);
}

void TestSinkCrossfade::testFlushAbortsStagedCrossfadeCarriesPrimaryPipe()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(sink.mixer.isCrossfading());

    // Seek-style flush (nullptr primaryPipe) before the crossfade latches:
    // carries pipeB over as the primary pipe and abandons the fade at unity gain.
    std::ignore = sink.mixer.stageFlush(nullptr, 0ms);
    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-6f);
    QVERIFY(!sink.mixer.isCrossfading());
}

void TestSinkCrossfade::testProcessFlushResetsFade()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(!sink.callback(2048).empty());

    // A seek-style flush (staged offset + deferred flush flags) must clear
    // the fade on the audio thread and retire the fading pipe.
    sink.clock.setPositionOffset(0ms);
    std::ignore = sink.mixer.stageFlush(nullptr, 0ms);
    sink.callback(2048); // processes the flush, draining both pipes
    QVERIFY(!sink.mixer.isCrossfading());

    // Fresh samples play at unity gain.
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-6f);
}

void TestSinkCrossfade::testFadeInOnlyWithDeferredFlush()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeB;
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    // DragonPlayer::setSource on track change: the deferred flush of the
    // previous switch is consumed by a callback, then the fade-in is armed
    // (the pipeline waits for the pending request before arming).
    sink.clock.setPositionOffset(0ms);
    std::ignore = sink.mixer.stageFlush(nullptr, 0ms);
    sink.callback(2048);
    QVERIFY(sink.mixer.armCrossfade(nullptr, &pipeB, 0ms, 1000ms).has_value());

    const auto out = drainSpan(sink.callback(2048));
    QCOMPARE(out.size(), size_t{2048});
    // In a fade-in from silence, the first frame MUST start at silence: sin(0) = 0.
    QVERIFY2(qAbs(out[0]) < 1e-4f,
             qPrintable(QStringLiteral("Expected fade-in starting at silence (~0.0), but got %1 (fade-in was wiped out by flush)").arg(out[0])));
}

void TestSinkCrossfade::testRapidRetrigger2SlotHandoff()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> slot0;
    Pipe<float> slot1;

    writeAll(slot0, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(slot1, makeConstant(0.5f, kTestFrames, kTestChannels));

    // Arm and latch A -> B: fading = slot0, primary = slot1
    QVERIFY(sink.mixer.armCrossfade(&slot0, &slot1, 1000ms, 1000ms).has_value());
    sink.callback(2048);
    QVERIFY(sink.mixer.isCrossfading());

    // Track change during running crossfade:
    // stageFlush retires/drains slot0 on the audio thread and hard-switches at 100% volume.
    std::ignore = sink.mixer.stageFlush(&slot1, 0ms);

    // Render: drains outgoing slot0 and plays slot1's FlushTail, leaving slot1 ready for new track
    const auto flushOut = drainSpan(sink.callback(2048));
    QVERIFY(!flushOut.empty());
    QVERIFY(!sink.mixer.isCrossfading());
    QCOMPARE(slot0.consumer().ready(), size_t{0});
    QCOMPARE(slot1.consumer().ready(), size_t{0});

    writeAll(slot1, makeConstant(0.7f, kTestFrames, kTestChannels));
    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    QCOMPARE(out[0], 0.7f);
}

void TestSinkCrossfade::testFlushAbortsStagedCrossfadeWithExplicitPipe()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<float> pipeC;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeC, makeConstant(0.7f, kTestFrames, kTestChannels));

    // Stage A -> B crossfade (not yet latched by audio thread)
    sink.mixer.setAudioPipe(&pipeA);
    std::ignore = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(sink.mixer.isCrossfadePending());
    QVERIFY(sink.mixer.isCrossfading());

    // Hard-switch flush to pipeC supersedes the un-latched crossfade
    std::ignore = sink.mixer.stageFlush(&pipeC, 0ms);

    // Render: FlushTail ramps 5 ms of pipeA, while pipeC plays at unity (0.7f)
    const auto out = drainSpan(sink.callback(2048));
    QVERIFY(!out.empty());
    QVERIFY(!sink.mixer.isCrossfading());
    QVERIFY2(qAbs(out.back() - 0.7f) < 1e-4f, qPrintable(QStringLiteral("Expected unity gain (0.7f) after flush supersede, but got %1").arg(out.back())));
}

void TestSinkCrossfade::testSeekGatingNearEofPreservesEos()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipe;
    sink.mixer.setAudioPipe(&pipe);
    writeAll(pipe, makeConstant(0.5f, 1024, 2));

    // Play initial samples
    auto out1 = sink.callback(2048);
    QCOMPARE(out1.size(), size_t{2048}); // 1024 frames * 2 ch

    // Now seek near EOF: stage a flush (gates the producer, minted epoch).
    sink.clock.setPositionOffset(5000ms);
    const auto seekEpoch = sink.mixer.stageFlush(nullptr, 0ms);
    QVERIFY(!sink.clock.isQueueReady());

    // Producer must wait for queueReady.
    // Audio callback consumes flush and calls beginStream():
    sink.callback(2048);
    QVERIFY(sink.clock.isQueueReady());

    // Post-seek: decode thread produces final 512 frames and announces EOS
    const auto postSeekEpoch = sink.clock.streamEpoch();
    QCOMPARE(postSeekEpoch, seekEpoch);
    writeAll(pipe, makeConstant(0.5f, 512, 2));
    sink.clock.samplesProduced(postSeekEpoch, 1024);
    sink.clock.boundaryEndOfStream(postSeekEpoch);

    // Consume post-seek samples
    auto out2 = sink.callback(2048);
    QCOMPARE(out2.size(), size_t{1024}); // 512 frames * 2 ch

    // All post-seek samples consumed, EOS must be pending and claimable
    QVERIFY(sink.clock.endOfStreamPending());
    QVERIFY(sink.clock.tryClaimDrained());
}

void TestSinkCrossfade::testFadeInFromClosedPreservedAcrossFormatReopen()
{
    PlaybackPipeline pipeline(nullptr);
    Pipe<float> pipe;

    // Stage standalone fade-in while device is closed (sampleRate == 0),
    // matching DragonPlayer::setSource(url, fadeInOptions) from StoppedState.
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 1000ms);
    QVERIFY(!pipeline.isFlushPending());

    // Open device once decoder resolves format: resetChannels() must preserve
    // the unstarted fade-in and scale it to 44100 Hz.
    pipeline.open(44100, 2);
    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));

    const auto out = drainSpan(pipeline.sink()->mixer()->render(2048, 0us));
    QCOMPARE(out.size(), size_t{2048});
    QVERIFY2(qAbs(out[0]) < 1e-4f, "Fade-in staged while closed must start at silence after open()");
    QVERIFY(out.back() > 1e-3f);

    // Conversely, staging 0ms fade-in before open() clears any prior unstarted fade-in.
    pipeline.close();
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 1000ms);
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 0ms);
    pipeline.open(44100, 2);
    writeAll(pipe, makeConstant(0.5f, 2048, kTestChannels));

    const auto unityOut = drainSpan(pipeline.sink()->mixer()->render(2048, 0us));
    QCOMPARE(unityOut.size(), size_t{2048});
    QVERIFY(qAbs(unityOut[0] - 0.5f) < 1e-6f);
}

void TestSinkCrossfade::testCrossfadeSupersedesPendingFlushSingleBeginStream()
{
    TestMixer sink;
    sink.configure(44100, 2, 2048);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    sink.mixer.setAudioPipe(&pipeA);

    // Establish epoch 1 with some playback.
    sink.callback(2048);
    const auto epochBefore = sink.clock.streamEpoch();

    // Stage a seek-style flush, then arm a crossfade while the flush is still
    // pending. The single-outstanding policy refuses the arm; the pipeline
    // layer first waits for the flush to be consumed, then arms: each switch
    // publishes exactly one stream.
    std::ignore = sink.mixer.stageFlush(nullptr, 0ms);
    QVERIFY(!sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms).has_value());

    sink.callback(2048);
    QCOMPARE(sink.clock.streamEpoch(), epochBefore + 1);

    const auto armed = sink.mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(armed.has_value());
    QCOMPARE(*armed, epochBefore + 2); // the epoch minted at staging

    sink.callback(2048);
    QCOMPARE(sink.clock.streamEpoch(), epochBefore + 2);

    // The crossfade latched: primary is pipeB.
    QVERIFY(sink.mixer.isCrossfading());
}

QTEST_GUILESS_MAIN(TestSinkCrossfade)
#include "test_sink_crossfade.moc"
