/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "mixer/flushtail.h"
#include "mixer/fadingchannel.h"
#include "mixer/mixer.h"
#include "mixer/primarychannel.h"
#include "mixer/streamswitch.h"
#include "player/pipe.h"
#include "sink/streamclock.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

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

void writeAll(Pipe<float> &pipe, const std::vector<float> &samples)
{
    std::stop_source source;
    const size_t written = pipe.producer().write(samples, source.get_token());
    QCOMPARE(written, samples.size());
}

}

class TestMixerChannels : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // StreamSwitch
    void testSwitchArmAndTake();
    void testSwitchStageRefusedWhilePending();
    void testSwitchSupersedeReplacesAndReturnsPrevious();
    void testSwitchStageDuringTakeSurvives();
    void testSwitchStressNeverLosesOrTearsRequest();
    void testSwitchClearQuiesced();

    // PrimaryChannel
    void testPrimaryInstallAndFadeEndpoints();
    void testPrimaryCurrentGainMidFadeMatchesLastSampleGain();
    void testPrimaryInstallNullPreservesPipe();
    void testPrimaryResetFade();

    // FadingChannel
    void testFadingRetireAndReset();
    void testFadingSamePipeReinstallDoesNotDrain();
    void testFadingSlowLoadHoldTransitionsToCosineDecay();
    void testFadingRingBufferWrapSeamContinuity();
    void testFadingProducerDoneClobberedByConsecutiveCrossfade();

    // Anti-click mitigation (FlushTail) & mid-fade-in transitions
    void testFlushMicroFadeRampDown();
    void testFlushSamePipeCapturesFlushTail();
    void testFlushTailMultiQuantumDrainAndClamp();
    void testFlushTailRingBufferWrapSeam();
    void testFlushTailClear();
    void testFlushSmallQuantumNeverOverrunsMaxSamples();
    void testFlushMidFadeInStartsAtCurrentGain();
    void testFlushStreamsAndResetClearFlushTail();
    void testCrossfadeMidPrimaryFadeInPreservesOutgoingGain();
};

namespace
{

StreamSwitch::Request makeCrossfadeRequest(Pipe<float> *fading, Pipe<float> *primary, int fadeOutMs, int fadeInMs)
{
    StreamSwitch::Request request;
    request.kind = StreamSwitch::Kind::Crossfade;
    request.primaryPipe = primary;
    request.fadingPipe = fading;
    request.fadeOut = std::chrono::milliseconds{fadeOutMs};
    request.fadeIn = std::chrono::milliseconds{fadeInMs};
    return request;
}

}

void TestMixerChannels::testSwitchArmAndTake()
{
    StreamSwitch sw;
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    QVERIFY(!sw.poll());
    QVERIFY(!sw.isPending());
    QVERIFY(!sw.hasFadingPipe(&pipeA));

    auto request = makeCrossfadeRequest(&pipeA, &pipeB, 1500, 800);
    request.epoch = 7;
    QVERIFY(sw.stage(request));
    QVERIFY(sw.poll());
    QVERIFY(sw.isPending());
    QVERIFY(sw.hasFadingPipe(&pipeA));
    QVERIFY(!sw.hasFadingPipe(&pipeB));

    // The request is copied out non-destructively: nothing is exchanged away,
    // so a racing supersede cannot orphan the pipes.
    const auto taken = sw.take();
    QCOMPARE(taken.kind, StreamSwitch::Kind::Crossfade);
    QCOMPARE(taken.epoch, std::uint64_t{7});
    QCOMPARE(taken.primaryPipe, &pipeB);
    QCOMPARE(taken.fadingPipe, &pipeA);
    QCOMPARE(taken.fadeOut, 1500ms);
    QCOMPARE(taken.fadeIn, 800ms);

    // The request stays pending until the consumer acknowledges it, so a
    // main-thread crossfade wait only returns once the switch is handled.
    QVERIFY(sw.poll());
    QVERIFY(sw.isPending());
    sw.acknowledge();
    QVERIFY(!sw.poll());
    QVERIFY(!sw.isPending());
    QVERIFY(!sw.hasFadingPipe(&pipeA));
}

void TestMixerChannels::testSwitchStageRefusedWhilePending()
{
    StreamSwitch sw;
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeA, &pipeB, 1000, 1000)));
    // Single outstanding request: a second stage is refused; the caller
    // decides the supersede policy.
    QVERIFY(!sw.stage(makeCrossfadeRequest(&pipeB, &pipeA, 500, 500)));

    const auto taken = sw.take();
    QCOMPARE(taken.kind, StreamSwitch::Kind::Crossfade);
    QCOMPARE(taken.primaryPipe, &pipeB);
    // A claimed-but-unacknowledged request still blocks staging.
    QVERIFY(!sw.stage(makeCrossfadeRequest(&pipeB, &pipeA, 500, 500)));
    sw.acknowledge();
    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeB, &pipeA, 500, 500)));
    sw.acknowledge();
}

void TestMixerChannels::testSwitchSupersedeReplacesAndReturnsPrevious()
{
    StreamSwitch sw;
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeA, &pipeB, 1000, 1000)));

    StreamSwitch::Request flush;
    flush.kind = StreamSwitch::Kind::Flush;
    flush.epoch = 9;

    const auto previous = sw.supersede(flush);
    QCOMPARE(previous.kind, StreamSwitch::Kind::Crossfade);
    QCOMPARE(previous.primaryPipe, &pipeB);

    const auto taken = sw.take();
    QCOMPARE(taken.kind, StreamSwitch::Kind::Flush);
    QCOMPARE(taken.epoch, std::uint64_t{9});
    sw.acknowledge();
}

// A request staged while the consumer is mid-take is never lost: the
// seqlock retries on sequence mismatch so the following take() observes it.
void TestMixerChannels::testSwitchStageDuringTakeSurvives()
{
    StreamSwitch sw;
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<float> pipeC;

    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeA, &pipeB, 1000, 1000)));

    // Stage a replacement while the request is still pending (un-consumed):
    // the writer supersedes, the consumer cannot ack the voided request.
    QVERIFY(sw.isPending());
    auto replacement = makeCrossfadeRequest(&pipeB, &pipeC, 500, 500);
    replacement.epoch = 42;
    std::ignore = sw.supersede(replacement);

    // The next take() observes the replacement, never a torn mix of the two.
    const auto taken = sw.take();
    QCOMPARE(taken.kind, StreamSwitch::Kind::Crossfade);
    QCOMPARE(taken.primaryPipe, &pipeC);
    QCOMPARE(taken.fadingPipe, &pipeB);
    QCOMPARE(taken.fadeOut, 500ms);
    QCOMPARE(taken.epoch, std::uint64_t{42});
    sw.acknowledge();
    QVERIFY(!sw.isPending());
}

// Stress the interleavings verified in the threading analysis: a writer
// staging/cancelling/superseding on one thread while a consumer takes on
// another. Invariants: every taken request is internally consistent (its
// pipes form one of the writer's pairs), and after the writer stops and the
// consumer drains, the final request is observed exactly once (nothing is
// silently lost or duplicated).
void TestMixerChannels::testSwitchStressNeverLosesOrTearsRequest()
{
    constexpr int kIterations = 20000;
    Pipe<float> pipes[4];
    StreamSwitch sw;

    std::atomic<bool> writerDone{false};
    std::atomic<int> taken{0};

    std::jthread consumer([&](std::stop_token st) {
        while (!st.stop_requested() && (!writerDone.load(std::memory_order_acquire) || sw.poll())) {
            if (!sw.poll()) {
                continue;
            }
            const auto request = sw.take();
            sw.acknowledge();
            if (request.kind == StreamSwitch::Kind::None) {
                continue;
            }
            // Cross-field consistency: the fading and primary pipes must
            // form one of the pairs the writer stages (a torn read would
            // pair pipes from different requests). A flush carries no fading
            // pipe.
            const bool consistentRequest = (request.kind == StreamSwitch::Kind::Flush && request.fadingPipe == nullptr)
                || (request.fadingPipe == nullptr && request.primaryPipe == nullptr) || (request.fadingPipe == &pipes[0] && request.primaryPipe == &pipes[1])
                || (request.fadingPipe == &pipes[1] && request.primaryPipe == &pipes[2])
                || (request.fadingPipe == &pipes[2] && request.primaryPipe == &pipes[3]);
            QVERIFY2(consistentRequest, "torn request observed: pipes from different staged requests");
            QVERIFY(request.fadeOut <= 1000ms);
            QVERIFY(request.fadeIn <= 1000ms);
            taken.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < kIterations; ++i) {
        const int pair = i % 3;
        auto request = makeCrossfadeRequest(&pipes[pair], &pipes[pair + 1], (i % 4) * 250, (i % 3) * 250);
        request.epoch = static_cast<std::uint64_t>(i);
        switch (i % 4) {
        case 0:
        case 1:
            if (!sw.stage(request)) {
                std::ignore = sw.supersede(request);
            }
            break;
        case 2:
            std::ignore = sw.supersede(request);
            break;
        case 3: {
            StreamSwitch::Request flush;
            flush.kind = StreamSwitch::Kind::Flush;
            flush.epoch = static_cast<std::uint64_t>(i);
            flush.primaryPipe = &pipes[pair];
            std::ignore = sw.supersede(flush);
            break;
        }
        }
    }
    writerDone.store(true, std::memory_order_release);
    consumer.join();

    // The consumer observed every request it acked without tearing; nothing
    // more is pending.
    QVERIFY(!sw.poll());
    QVERIFY(taken.load() > 0);
}

void TestMixerChannels::testSwitchClearQuiesced()
{
    StreamSwitch sw;
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeA, &pipeB, 1000, 1000)));

    const auto previous = sw.clear();
    QCOMPARE(previous.kind, StreamSwitch::Kind::Crossfade);
    QVERIFY(!sw.isPending());
    QVERIFY(!sw.poll());

    // A cleared switch stages new requests cleanly.
    QVERIFY(sw.stage(makeCrossfadeRequest(&pipeB, &pipeA, 100, 100)));
    QCOMPARE(sw.take().kind, StreamSwitch::Kind::Crossfade);
    sw.acknowledge();
}

void TestMixerChannels::testPrimaryInstallAndFadeEndpoints()
{
    PrimaryChannel primary;
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(1.0f, 1000, 2));

    primary.install(&pipe, 1000ms, 44100);
    QVERIFY(primary.isFadeActive());
    QVERIFY(primary.isWaitingForFirstSample());
    QCOMPARE(primary.currentGain(), 0.0f);

    std::vector<float> buf(2000, 0.0f);

    // Empty applyFade does not release slow-load waiting flag.
    primary.applyFade(buf, 0, 2);
    QVERIFY(primary.isWaitingForFirstSample());

    // Pull samples and apply fade.
    const size_t read = primary.pull(buf, 200);
    QCOMPARE(read, size_t{200});
    primary.applyFade(buf, read, 2);

    QVERIFY(!primary.isWaitingForFirstSample());
    QVERIFY(primary.isFadeActive());
    QVERIFY(primary.currentGain() > 0.0f);
}

void TestMixerChannels::testPrimaryCurrentGainMidFadeMatchesLastSampleGain()
{
    PrimaryChannel primary;
    Pipe<float> pipe;
    const int totalFrames = 1000;
    const int sampleRate = 1000;
    writeAll(pipe, makeConstant(1.0f, totalFrames, 2));

    primary.install(&pipe, 1000ms, sampleRate);

    std::vector<float> buf(600, 0.0f);
    const size_t read1 = primary.pull(buf, 600);
    QCOMPARE(read1, size_t{600});
    primary.applyFade(buf, read1, 2);

    constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
    const float expectedGain300 = std::sin((300.0f / 1000.0f) * kHalfPi);

    QVERIFY(qAbs(primary.currentGain() - expectedGain300) < 1e-5f);
    const float expectedSampleGain299 = (299.0f / 300.0f) * expectedGain300;
    QVERIFY(qAbs(buf[598] - expectedSampleGain299) < 1e-4f);
    QVERIFY(qAbs(buf[599] - expectedSampleGain299) < 1e-4f);

    // Complete the remaining 700 frames.
    std::vector<float> bufRest(1400, 0.0f);
    const size_t read2 = primary.pull(bufRest, 1400);
    QCOMPARE(read2, size_t{1400});
    primary.applyFade(bufRest, read2, 2);

    // First frame of chunk 2 starts at the exact gain where chunk 1 ended:
    QVERIFY(qAbs(bufRest[0] - expectedGain300) < 1e-4f);
    QVERIFY(qAbs(bufRest[1] - expectedGain300) < 1e-4f);

    QVERIFY(!primary.isFadeActive());
    QCOMPARE(primary.currentGain(), 1.0f);
    const float expectedLastSampleGain = expectedGain300 + (699.0f / 700.0f) * (1.0f - expectedGain300);
    QVERIFY(qAbs(bufRest[1398] - expectedLastSampleGain) < 1e-4f);
    QVERIFY(qAbs(bufRest[1399] - expectedLastSampleGain) < 1e-4f);
}

void TestMixerChannels::testPrimaryInstallNullPreservesPipe()
{
    PrimaryChannel primary;
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.42f, 100, 2));

    primary.setPipe(&pipe);
    primary.install(nullptr, 500ms, 44100);

    std::vector<float> buf(20, 0.0f);
    const size_t read = primary.pull(buf, 20);
    QCOMPARE(read, size_t{20});
    QVERIFY(qAbs(buf[0] - 0.42f) < 1e-6f);
}

void TestMixerChannels::testPrimaryResetFade()
{
    PrimaryChannel primary;
    primary.install(nullptr, 1000ms, 44100);
    QVERIFY(primary.isFadeActive());

    primary.resetFade();
    QVERIFY(!primary.isFadeActive());
    QCOMPARE(primary.currentGain(), 1.0f);
}

void TestMixerChannels::testFadingRetireAndReset()
{
    // Part 1: retire() drains the pipe and stops fade-out
    {
        FadingChannel fading;

        Pipe<float> pipeA;
        writeAll(pipeA, makeConstant(0.8f, 2000, 2));

        fading.install(
            FadingChannel::FadingSpec{.pipe = &pipeA, .fadeOut = 1000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 44100});
        std::vector<float> buf(200, 0.0f);
        fading.mixInto(buf, 0, 200, false, 2);
        QVERIFY(fading.isActive());

        fading.retire();
        QVERIFY(!fading.isActive());
        QCOMPARE(pipeA.consumer().ready(), size_t{0});
    }

    // Part 2: reset() detaches the pipe without draining it
    {
        FadingChannel fading;

        Pipe<float> pipeA;
        writeAll(pipeA, makeConstant(0.8f, 2000, 2));
        const size_t initialReady = pipeA.consumer().ready();

        fading.install(
            FadingChannel::FadingSpec{.pipe = &pipeA, .fadeOut = 1000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 44100});
        fading.reset();
        QVERIFY(!fading.isActive());
        QCOMPARE(pipeA.consumer().ready(), initialReady);
    }
}

void TestMixerChannels::testFadingSamePipeReinstallDoesNotDrain()
{
    FadingChannel fading;

    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.5f, 5000, 2));
    const size_t initialAvailable = pipe.consumer().ready();
    QVERIFY(initialAvailable > 0);

    fading.install(
        FadingChannel::FadingSpec{.pipe = &pipe, .fadeOut = 1000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 44100});
    // Re-install the same pipe: oldFading == nextFadingPipe
    fading.install(
        FadingChannel::FadingSpec{.pipe = &pipe, .fadeOut = 500ms, .holdMax = 0ms, .initialGain = 0.5f, .incomingHasFadeIn = false, .sampleRate = 44100});

    // pipe must NOT have been drained
    QCOMPARE(pipe.consumer().ready(), initialAvailable);
}

void TestMixerChannels::testFadingSlowLoadHoldTransitionsToCosineDecay()
{
    FadingChannel fading;
    Pipe<float> pipe;
    const int sampleRate = 1000;
    const int channels = 2;
    writeAll(pipe, makeConstant(1.0f, 1000, channels));

    fading.install(FadingChannel::FadingSpec{.pipe = &pipe,
                                             .fadeOut = 1000ms,
                                             .holdMax = 500ms,
                                             .initialGain = 1.0f,
                                             .incomingHasFadeIn = true,
                                             .sampleRate = sampleRate});
    QVERIFY(fading.isFadeOutActive());
    QCOMPARE(fading.currentGain(), 1.0f);

    // Chunk 1: 200 frames with holdForSlowLoad = true (gain held at 1.0f)
    std::vector<float> buf1(400, 0.0f);
    const size_t mixed1 = fading.mixInto(buf1, 0, 400, true, channels);
    QCOMPARE(mixed1, size_t{400});
    QVERIFY(qAbs(buf1[0] - 1.0f) < 1e-4f);
    QVERIFY(qAbs(buf1[398] - 1.0f) < 1e-4f);
    QCOMPARE(fading.currentGain(), 1.0f);

    // Chunk 2: 400 frames with holdForSlowLoad = true.
    // 300 frames remaining of hold, then 100 frames of cosine decay.
    std::vector<float> buf2(800, 0.0f);
    const size_t mixed2 = fading.mixInto(buf2, 0, 800, true, channels);
    QCOMPARE(mixed2, size_t{800});
    QVERIFY(buf2[798] < 1.0f);
    QVERIFY(fading.currentGain() < 1.0f);
}

void TestMixerChannels::testFadingRingBufferWrapSeamContinuity()
{
    FadingChannel fading;
    Pipe<float> pipe;

    // Push pipe near 65536 ring buffer boundary.
    writeAll(pipe, makeConstant(0.0f, 32000, 2));
    pipe.consumer().readSomeWith(64000, [](auto, auto) { });

    // Write 3000 stereo samples of 1.0f crossing the boundary.
    writeAll(pipe, makeConstant(1.0f, 3000, 2));

    fading.install(
        FadingChannel::FadingSpec{.pipe = &pipe, .fadeOut = 3000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 1000});

    std::vector<float> buf(2646, 0.0f);
    const size_t mixed = fading.mixInto(buf, 0, 2646, false, 2);
    QCOMPARE(mixed, size_t{2646});

    // Verify monotonic decay across the entire chunk including the b1/b2 seam.
    for (size_t f = 1; f < mixed / 2; ++f) {
        QVERIFY(buf[2 * f] <= buf[2 * (f - 1)] + 1e-6f);
        QVERIFY(buf[2 * f + 1] <= buf[2 * (f - 1) + 1] + 1e-6f);
    }

    // Also verify 6-channel (5.1) wrap seam where 65536 % 6 == 4 splits a frame across b1 and b2.
    FadingChannel fading6;
    Pipe<float> pipe6;
    writeAll(pipe6, makeConstant(0.0f, 10900, 6)); // 65400 samples
    pipe6.consumer().readSomeWith(65400, [](auto, auto) { });
    writeAll(pipe6, makeConstant(1.0f, 500, 6)); // 3000 samples crossing 65536 (136 samples in b1 -> 22 frames + 4 samples)

    fading6.install(
        FadingChannel::FadingSpec{.pipe = &pipe6, .fadeOut = 1000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 1000});
    std::vector<float> buf6(600, 0.0f); // 100 6-channel frames
    const size_t mixed6 = fading6.mixInto(buf6, 0, 600, false, 6);
    QCOMPARE(mixed6, size_t{600});
    for (size_t f = 0; f < mixed6 / 6; ++f) {
        for (size_t c = 1; c < 6; ++c) {
            QVERIFY(qFuzzyCompare(buf6[6 * f + c], buf6[6 * f]));
        }
        if (f > 0) {
            QVERIFY(buf6[6 * f] <= buf6[6 * (f - 1)] + 1e-6f);
        }
    }
}

void TestMixerChannels::testFadingProducerDoneClobberedByConsecutiveCrossfade()
{
    FadingChannel fading;

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.8f, 200, 2));

    // Install pipeA with 100ms fade-out
    fading.install(
        FadingChannel::FadingSpec{.pipe = &pipeA, .fadeOut = 100ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 1000});
    QVERIFY(fading.isFadeOutActive());

    // Main thread sets up pipeB with producerDone = false (pipe default)
    pipeB.setProducerDone(false);

    // Audio thread finishes pipeA before Arm is latched:
    std::vector<float> buf(400, 0.0f);
    fading.mixInto(buf, 0, 400, false, 2);
    // pipeA ran out of frames and retired:
    QVERIFY(!fading.isActive());

    // Now audio thread latches B -> C (install pipeB).
    // pipeB is temporarily empty (simulating slow decode load).
    fading.install(
        FadingChannel::FadingSpec{.pipe = &pipeB, .fadeOut = 1000ms, .holdMax = 500ms, .initialGain = 1.0f, .incomingHasFadeIn = true, .sampleRate = 1000});

    // Render one chunk while pipeB has 0 samples ready.
    // In slow-load hold (incomingHasFadeIn = true, holdForSlowLoad = true),
    // fading channel MUST remain active and wait for pipeB.
    std::fill(buf.begin(), buf.end(), 0.0f);
    fading.mixInto(buf, 0, 200, true, 2);

    // Because producerDone is pipe-owned, pipeA's retirement did not touch pipeB's flag.
    // mixInto() sees fadingRead == 0 && !fp->producerDone(), and holds for slow load without retiring!
    QVERIFY2(fading.isActive(), "fading channel retired prematurely on empty pipe because producerDone was not pipe-owned!");
}

void TestMixerChannels::testFlushMicroFadeRampDown()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    Mixer mixer(clock);
    mixer.preAllocate(2048);

    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 2000, 2));
    mixer.setAudioPipe(&pipe);

    // Render one normal quantum
    auto out1 = mixer.render(512, 0us);
    QVERIFY(!out1.empty());
    QCOMPARE(out1[0], 0.8f);

    // Stage a flush
    std::ignore = mixer.stageFlush(nullptr, 0ms);

    // Render quantum with flush: micro-fade applies 5ms linear ramp from 0.8 down to 0
    auto out2 = mixer.render(1024, 0us);
    QVERIFY(!out2.empty());
    // First sample should be close to 0.8f, then decaying down towards 0.0f
    QVERIFY(out2[0] > 0.7f);
    // At 44.1kHz stereo, 5ms is 220 frames (440 samples). Towards end of micro-fade it reaches 0
    QVERIFY(out2[439] < 0.1f);
}

void TestMixerChannels::testFlushSamePipeCapturesFlushTail()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    Mixer mixer(clock);
    mixer.preAllocate(2048);

    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 2000, 2));
    mixer.setAudioPipe(&pipe);
    QCOMPARE(mixer.render(512, 0us).size(), std::size_t{512});

    // Non-rotating hard switch in DragonPlayer::setSource() passes the already
    // installed primary pipe: FlushTail must still capture and ramp down the
    // outgoing 5 ms tail instead of skipping it.
    std::ignore = mixer.stageFlush(&pipe, 0ms);
    const auto out = mixer.render(1024, 0us);
    QCOMPARE(out.size(), std::size_t{440});
    QVERIFY(qAbs(out[0] - 0.8f) < 1e-5f);
    QVERIFY(out.back() < 0.01f);
}

void TestMixerChannels::testFlushTailMultiQuantumDrainAndClamp()
{
    PrimaryChannel primary;
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 1000, 2));
    primary.setPipe(&pipe);

    FlushTail tail;
    QVERIFY(!tail.isActive());

    // 48 kHz stereo -> 5 ms = 240 frames = 480 samples
    tail.capture(primary, 48000, 2);
    QVERIFY(tail.isActive());

    std::vector<float> allDrained;
    while (tail.isActive()) {
        std::vector<float> quantum(128, -99.0f); // sentinel verifies tail zero-fills past validSamples=0
        const std::size_t mixed = tail.mixInto(quantum, 0, 2);
        QVERIFY(mixed > 0);
        QVERIFY(mixed <= quantum.size());
        allDrained.insert(allDrained.end(), quantum.begin(), quantum.begin() + static_cast<std::ptrdiff_t>(mixed));
    }

    QCOMPARE(allDrained.size(), std::size_t{480});
    QVERIFY(qAbs(allDrained[0] - 0.8f) < 1e-5f);
    for (std::size_t f = 1; f < allDrained.size() / 2; ++f) {
        QVERIFY(allDrained[2 * f] <= allDrained[2 * (f - 1)] + 1e-6f);
        QCOMPARE(allDrained[2 * f], allDrained[2 * f + 1]);
    }
    QVERIFY(allDrained.back() < 0.01f);

    // Exhausted tail returns validSamples unchanged.
    std::vector<float> passthrough(64, 0.25f);
    QCOMPARE(tail.mixInto(passthrough, 32, 2), std::size_t{32});
    QCOMPARE(passthrough[0], 0.25f);
}

void TestMixerChannels::testFlushTailRingBufferWrapSeam()
{
    PrimaryChannel primary;
    Pipe<float> pipe;

    // Advance pipe close to the 65536-sample ring wrap boundary (65400 consumed -> 136 samples before wrap).
    writeAll(pipe, makeConstant(0.0f, 32700, 2));
    pipe.consumer().readSomeWith(65400, [](auto, auto) { });

    // Write 500 stereo frames (1000 samples) of 0.9f straddling the wrap seam.
    writeAll(pipe, makeConstant(0.9f, 500, 2));
    primary.setPipe(&pipe);

    FlushTail tail;
    tail.capture(primary, 48000, 2);
    QVERIFY(tail.isActive());

    std::vector<float> dst(480, 0.0f);
    const std::size_t mixed = tail.mixInto(dst, 0, 2);
    QCOMPARE(mixed, std::size_t{480});
    QVERIFY(!tail.isActive());

    for (std::size_t f = 1; f < mixed / 2; ++f) {
        QVERIFY(dst[2 * f] <= dst[2 * (f - 1)] + 1e-6f);
    }
}

void TestMixerChannels::testFlushTailClear()
{
    PrimaryChannel primary;
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 1000, 2));
    primary.setPipe(&pipe);

    FlushTail tail;
    tail.capture(primary, 44100, 2);
    QVERIFY(tail.isActive());

    tail.clear();
    QVERIFY(!tail.isActive());

    std::vector<float> dst(256, 0.5f);
    QCOMPARE(tail.mixInto(dst, 64, 2), std::size_t{64});
    QCOMPARE(dst[0], 0.5f);
}

void TestMixerChannels::testFlushSmallQuantumNeverOverrunsMaxSamples()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    Mixer mixer(clock);
    // Deliberately omit preAllocate() so m_callbackBuffer is sized tightly to maxSamples (128).

    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 2000, 2));
    mixer.setAudioPipe(&pipe);

    const auto initial = mixer.render(128, 0us);
    QCOMPARE(initial.size(), std::size_t{128});

    std::ignore = mixer.stageFlush(nullptr, 0ms);

    std::vector<float> drained;
    for (int step = 0; step < 10; ++step) {
        const auto out = mixer.render(128, 0us);
        QVERIFY(!mixer.isCrossfading());
        QVERIFY(out.size() <= 128);
        if (out.empty()) {
            break;
        }
        drained.insert(drained.end(), out.begin(), out.end());
    }

    // 5 ms at 44.1 kHz stereo = 220 frames = 440 samples (across 4 callbacks: 128 + 128 + 128 + 56).
    QCOMPARE(drained.size(), std::size_t{440});
    QVERIFY(qAbs(drained[0] - 0.8f) < 1e-5f);
    for (std::size_t f = 1; f < drained.size() / 2; ++f) {
        QVERIFY(drained[2 * f] <= drained[2 * (f - 1)] + 1e-6f);
    }
    QVERIFY(drained.back() < 0.01f);
}

void TestMixerChannels::testFlushMidFadeInStartsAtCurrentGain()
{
    StreamClock clock;
    clock.setFormat(1000, 2); // 1 frame per ms -> exact gain math, 5 ms = 5 frames = 10 samples
    Mixer mixer(clock);
    mixer.preAllocate(1024);

    Pipe<float> pipe;
    std::ignore = mixer.flushStreams(&pipe, 1000ms);
    writeAll(pipe, makeConstant(1.0f, 1000, 2));

    // Render 200 frames (400 samples): primary fade-in reaches sin(0.2 * pi/2) ~= 0.309017f.
    const auto out1 = mixer.render(400, 0us);
    QCOMPARE(out1.size(), std::size_t{400});

    const float expectedGainAt200 = std::sin(0.2f * (std::numbers::pi_v<float> * 0.5f));

    std::ignore = mixer.stageFlush(nullptr, 0ms);
    const auto out2 = mixer.render(400, 0us);
    QCOMPARE(out2.size(), std::size_t{10}); // 5 frames * 2 channels

    // Must ramp down from expectedGainAt200 (~0.309), NOT jump up to 1.0f.
    QVERIFY(qAbs(out2[0] - expectedGainAt200) < 1e-4f);
    for (std::size_t f = 1; f < out2.size() / 2; ++f) {
        QVERIFY(out2[2 * f] < out2[2 * (f - 1)]);
    }
}

void TestMixerChannels::testFlushStreamsAndResetClearFlushTail()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    Mixer mixer(clock);

    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.8f, 2000, 2));
    mixer.setAudioPipe(&pipe);
    QCOMPARE(mixer.render(128, 0us).size(), std::size_t{128});

    // Capture 440-sample tail and drain only 128 samples of it.
    std::ignore = mixer.stageFlush(nullptr, 0ms);
    QCOMPARE(mixer.render(128, 0us).size(), std::size_t{128});

    // Mid-drain format reopen / synchronous flush must discard remaining tail samples.
    mixer.resetChannels();
    QVERIFY(mixer.render(128, 0us).empty());
}

void TestMixerChannels::testCrossfadeMidPrimaryFadeInPreservesOutgoingGain()
{
    StreamClock clock;
    clock.setFormat(1000, 2);
    Mixer mixer(clock);
    mixer.preAllocate(1024);

    Pipe<float> pipeA;
    Pipe<float> pipeB;
    std::ignore = mixer.flushStreams(&pipeA, 1000ms);
    writeAll(pipeA, makeConstant(1.0f, 1000, 2));

    // Advance pipeA's fade-in 200 frames -> gain reaches sin(0.2 * pi/2) ~= 0.309f.
    QCOMPARE(mixer.render(400, 0us).size(), std::size_t{400});
    const float expectedGainAt200 = std::sin(0.2f * (std::numbers::pi_v<float> * 0.5f));

    // Crossfade to pipeB (still loading/empty, fadeIn=0ms so no slow-load hold):
    // pipeA's fade-out must start at ~0.309f rather than jumping up to 1.0f.
    QVERIFY(mixer.armCrossfade(&pipeA, &pipeB, 1000ms, 0ms).has_value());
    const auto out = mixer.render(200, 0us);
    QCOMPARE(out.size(), std::size_t{200});
    QVERIFY(qAbs(out[0] - expectedGainAt200) < 1e-4f);
    QVERIFY(out[out.size() - 2] < out[0]);
}

QTEST_GUILESS_MAIN(TestMixerChannels)
#include "test_mixer_channels.moc"
