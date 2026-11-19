/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "player/pipe.h"
#include "voice/voice.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

using namespace std::chrono_literals;

namespace
{

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

std::unique_ptr<Voice> makeVoice(Pipe<float> *pipe, std::uint64_t generation, Voice::Notifiers notifiers = {})
{
    return std::make_unique<Voice>(pipe, generation, std::move(notifiers));
}

Voice::Notifiers fadingFinishedNotifiers(std::atomic<bool> &flag)
{
    Voice::Notifiers notifiers;
    notifiers.fadingFinished = [&flag](std::uint64_t) {
        flag.store(true, std::memory_order_release);
    };
    return notifiers;
}

}

// The voice render model can be driven deterministically on the test thread:
// Voice implements the AudioSinkStreamRenderer interface a backend stream
// pulls, so the crossfade/fade-in/seek logic is observable without any
// audio backend. There is no mixer: a crossfade here is two voices, the
// outgoing one demoted with a fade-out request, the incoming one owning the
// CrossfadeLink.
class TestVoice : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLatchHasZeroSwapWindowDropout();
    void testFadeInOnly();
    void testTransientStarvationVsRetirement();
    void testProducerDoneRetiresOnEmpty();
    void testSlowLoadEnvelopeHold();
    void testProcessFlushResetsFade();
    void testSeekGatingNearEos();
    void testCrossfadeMidPrimaryFadeInPreservesOutgoingGain();
    void testFadeCompletionReleasesUnderrunSuppression();
    void testFadingVoiceFormatStallTerminatesCleanly();
    void testDemotedVoiceDrainsStagedFlushBeforeFadeOut();
    void testPackedFlushStagingAndOverwrite();
};

void TestVoice::testLatchHasZeroSwapWindowDropout()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));

    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(0ms);

    auto outgoing = makeVoice(&pipeA, 1);
    outgoing->setFormat(44100, kTestChannels);
    std::ignore = outgoing->publishInitialStream(0ms);
    QVERIFY(!outgoing->render(2048).empty());

    // Fade-out only (no fade-in, no slow-load hold): the very first render
    // after demotion must already produce the outgoing voice's fade-out
    // samples (no one-quantum dropout, no false underrun on the incoming
    // voice) even though the incoming pipe is still empty.
    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, false);

    const auto out = drainSpan(outgoing->render(2048));
    QVERIFY(!out.empty());
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-6f);
    QVERIFY(out[out.size() - 2] < out[0]);

    // The incoming voice renders silence without counting an underrun while
    // the outgoing voice is still fading (outgoingActive suppresses it).
    QVERIFY(incoming->render(2048).empty());
    QCOMPARE(incoming->clock().underrunCount(), 0);
}

void TestVoice::testFadeInOnly()
{
    Pipe<float> pipeB;
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    auto incoming = makeVoice(&pipeB, 1);
    incoming->setFormat(44100, kTestChannels);
    std::ignore = incoming->publishInitialStream(0ms);
    // The pending fade-in applies once the native sample rate is known (the
    // pipeline does this at open()/installStream time).
    incoming->installStream(nullptr, 1000ms);

    const auto out = drainSpan(incoming->render(2048));
    QCOMPARE(out.size(), size_t{2048});
    // First frame of the fade-in starts at silence (sin(0) = 0).
    QVERIFY(qAbs(out[0]) < 1e-6f);
    QVERIFY(qAbs(out[out.size() - 1]) > 1e-3f);
    QCOMPARE(incoming->clock().underrunCount(), 0);
}

void TestVoice::testTransientStarvationVsRetirement()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    // The retiring pipe holds only a short tail so it drains mid-fade.
    writeAll(pipeA, makeConstant(0.5f, 2048, kTestChannels));

    std::atomic<bool> finished{false};
    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    auto outgoing = makeVoice(&pipeA, 1, fadingFinishedNotifiers(finished));
    outgoing->setFormat(44100, kTestChannels);
    std::ignore = outgoing->publishInitialStream(0ms);

    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, true);
    QVERIFY(!outgoing->render(2048).empty());

    // The retiring producer has NOT finished: even once its pipe runs dry the
    // fade-out advances over real callback quanta (the hold cannot deadlock
    // behind a stalled producer) and the voice stays unfinished.
    while (pipeA.consumer().ready() > 0) {
        std::ignore = outgoing->render(2048);
    }
    for (int i = 0; i < 3; ++i) {
        std::ignore = outgoing->render(2048);
    }
    QVERIFY(!finished.load(std::memory_order_acquire));

    // Once the producer feeds the pipe again the fade-out resumes.
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    QVERIFY(!outgoing->render(2048).empty());
    QVERIFY(!finished.load(std::memory_order_acquire));
}

void TestVoice::testProducerDoneRetiresOnEmpty()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, 2048, kTestChannels));
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::atomic<bool> finished{false};
    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    auto outgoing = makeVoice(&pipeA, 1, fadingFinishedNotifiers(finished));
    outgoing->setFormat(44100, kTestChannels);
    std::ignore = outgoing->publishInitialStream(0ms);

    // The fade-out lasts 1000 ms but pipeA only holds ~23 ms: once it drains
    // and the producer is done, the voice must finish without waiting for
    // the full fade window.
    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, false);
    pipeA.setProducerDone(true);

    for (int i = 0; i < 600 && !finished.load(std::memory_order_acquire); ++i) {
        std::ignore = outgoing->render(2048);
    }
    QVERIFY(finished.load(std::memory_order_acquire));
}

void TestVoice::testSlowLoadEnvelopeHold()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));

    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(1000ms);
    auto outgoing = makeVoice(&pipeA, 1);
    outgoing->setFormat(44100, kTestChannels);
    std::ignore = outgoing->publishInitialStream(0ms);

    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, true);

    // While the incoming track has not produced a sample the outgoing gain
    // must hold instead of ramping to silence before it ever starts.
    const auto held = drainSpan(outgoing->render(2048));
    QVERIFY(!held.empty());
    QVERIFY(qAbs(held[0] - 0.5f) < 1e-6f);

    // Feed the incoming track: its first non-empty render releases the hold.
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    incoming->setFormat(44100, kTestChannels);
    incoming->installStream(nullptr, 1000ms);
    QVERIFY(!incoming->render(2048).empty());

    const auto out = drainSpan(outgoing->render(2048));
    QVERIFY(!out.empty());
    // The fade-out resumes from the held gain (not collapsed by the stall)
    // and is falling again.
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-4f);
    QVERIFY(out[out.size() - 2] < out[0]);
}

void TestVoice::testProcessFlushResetsFade()
{
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));

    auto primary = makeVoice(&pipe, 1);
    primary->setFormat(44100, kTestChannels);
    std::ignore = primary->publishInitialStream(0ms);
    primary->installStream(nullptr, 1000ms);
    QVERIFY(!primary->render(2048).empty());
    QVERIFY(primary->clock().totalSamplesWritten() > 0);

    // A seek-style flush (staged offset + deferred flags) must drain the
    // pipe, clear the fade-in on the audio thread, and publish the new
    // stream; fresh samples then play at unity gain.
    primary->clock().setPositionOffset(0ms);
    std::ignore = primary->stageFlush(0ms);
    std::ignore = primary->render(2048);
    QCOMPARE(pipe.consumer().ready(), size_t{0});

    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));
    const auto out = drainSpan(primary->render(2048));
    QVERIFY(!out.empty());
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-6f);
}

void TestVoice::testSeekGatingNearEos()
{
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.5f, 1024, 2));

    auto primary = makeVoice(&pipe, 1);
    primary->setFormat(44100, 2);
    std::ignore = primary->publishInitialStream(0ms);

    const auto out1 = drainSpan(primary->render(2048));
    QCOMPARE(out1.size(), size_t{2048});

    // Now seek near EOF: stage a flush (gates the producer, mints the epoch).
    primary->clock().setPositionOffset(5000ms);
    const auto seekEpoch = primary->stageFlush(0ms);
    QVERIFY(!primary->clock().isQueueReady());

    // The render callback consumes the flush and calls beginStream(): the
    // producer gate reopens.
    std::ignore = primary->render(2048);
    QVERIFY(primary->clock().isQueueReady());

    const auto postSeekEpoch = primary->clock().streamEpoch();
    QCOMPARE(postSeekEpoch, seekEpoch);
    writeAll(pipe, makeConstant(0.5f, 512, 2));
    primary->clock().samplesProduced(postSeekEpoch, 1024);
    primary->clock().boundaryEndOfStream(postSeekEpoch);

    const auto out2 = drainSpan(primary->render(2048));
    QCOMPARE(out2.size(), size_t{1024});

    // All post-seek samples consumed, EOS must be pending and claimable.
    QVERIFY(primary->endOfStreamPending());
    QVERIFY(primary->tryClaimDrained());
}

void TestVoice::testCrossfadeMidPrimaryFadeInPreservesOutgoingGain()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(1.0f, 1000, 2));

    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(0ms);

    auto outgoing = makeVoice(&pipeA, 1);
    outgoing->setFormat(1000, 2); // 1 frame per ms -> exact gain math
    std::ignore = outgoing->publishInitialStream(0ms);
    outgoing->installStream(nullptr, 1000ms);

    // Advance the fade-in 200 frames -> gain reaches sin(0.2 * pi/2) ~= 0.309.
    QCOMPARE(drainSpan(outgoing->render(400)).size(), size_t{400});
    const float expectedGainAt200 = std::sin(0.2f * (std::numbers::pi_v<float> * 0.5f));

    // Crossfade to pipeB (still loading/empty, fadeIn 0 so no slow-load
    // hold): pipeA's fade-out must start at ~0.309 rather than jumping up
    // to 1.0 (a partially complete fade-in is preserved through its
    // mirrored current gain).
    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, false);

    const auto out = drainSpan(outgoing->render(200));
    QCOMPARE(out.size(), size_t{200});
    QVERIFY(qAbs(out[0] - expectedGainAt200) < 1e-4f);
    QVERIFY(out[out.size() - 2] < out[0]);
}

void TestVoice::testFadeCompletionReleasesUnderrunSuppression()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));

    std::atomic<bool> finished{false};
    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(0ms);
    auto outgoing = makeVoice(&pipeA, 1, fadingFinishedNotifiers(finished));
    outgoing->setFormat(44100, kTestChannels);
    std::ignore = outgoing->publishInitialStream(0ms);

    outgoing->demoteToFading(incoming->crossfadeLink(), 500ms, false);

    // Drain the whole fade-out (500 ms = 22050 frames = 44100 samples; each
    // render consumes up to 2048 samples and the starvation path advances
    // the envelope on real quanta once the pipe is dry).
    for (int i = 0; i < 50 && !finished.load(std::memory_order_acquire); ++i) {
        std::ignore = outgoing->render(2048);
    }
    QVERIFY(finished.load(std::memory_order_acquire));

    // The link's outgoing voice is done: the starving incoming primary's
    // underruns count again (outgoingActive suppression released).
    QVERIFY(incoming->render(2048).empty());
    QCOMPARE(incoming->clock().underrunCount(), 1);
}

void TestVoice::testFadingVoiceFormatStallTerminatesCleanly()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    std::atomic<bool> finished{false};
    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(0ms);

    auto outgoing = makeVoice(&pipeA, 1, fadingFinishedNotifiers(finished));
    outgoing->setFormat(44100, kTestChannels);
    const auto epoch = outgoing->publishInitialStream(0ms);

    // 1024 stereo frames = 2048 samples at 44100 Hz followed by a 48000 Hz boundary
    // and buffered samples of the next track so the pipe is non-empty at the boundary.
    writeAll(pipeA, makeConstant(0.5f, 1024, kTestChannels));
    outgoing->clock().samplesProduced(epoch, 2048);
    QVERIFY(outgoing->clock().boundaryTrackStart(2, 48000, kTestChannels, 1000ms, epoch));
    writeAll(pipeA, makeConstant(0.25f, 1024, kTestChannels));
    outgoing->clock().samplesProduced(epoch, 2048);

    QCOMPARE(drainSpan(outgoing->render(1024)).size(), size_t{1024});

    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, false);
    QVERIFY(incoming->crossfadeLink()->outgoingActive.load(std::memory_order_acquire));

    QCOMPARE(drainSpan(outgoing->render(2048)).size(), size_t{1024});
    QVERIFY(!finished.load(std::memory_order_acquire));

    // Next pull hits BoundaryAdvance::FormatStall on Role::Fading: must finish
    // cleanly, emit fadingFinished, and release outgoingActive on the link.
    QVERIFY(outgoing->render(2048).empty());
    QVERIFY(finished.load(std::memory_order_acquire));
    QVERIFY(!incoming->crossfadeLink()->outgoingActive.load(std::memory_order_acquire));
    QVERIFY(incoming->render(2048).empty());
    QCOMPARE(incoming->clock().underrunCount(), 1);
}

void TestVoice::testDemotedVoiceDrainsStagedFlushBeforeFadeOut()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;

    auto incoming = makeVoice(&pipeB, 2);
    std::ignore = incoming->publishInitialStream(0ms);
    incoming->setPendingFadeIn(0ms);

    auto outgoing = makeVoice(&pipeA, 1);
    outgoing->setFormat(44100, kTestChannels);
    const auto epoch1 = outgoing->publishInitialStream(0ms);

    // Stale pre-seek audio (0.25f) sitting in the outgoing pipe when a seek
    // flush is staged immediately before demotion to Role::Fading.
    writeAll(pipeA, makeConstant(0.25f, 1024, kTestChannels));
    outgoing->clock().samplesProduced(epoch1, 2048);
    const auto epoch2 = outgoing->stageFlush(0ms);
    QVERIFY(outgoing->isFlushPending());
    QVERIFY(!outgoing->clock().isQueueReady());

    outgoing->demoteToFading(incoming->crossfadeLink(), 1000ms, false);

    // First render on the fading voice must consume the pending flush, drain all
    // stale pre-seek samples, publish epoch2, and open the producer gate.
    QVERIFY(outgoing->render(2048).empty());
    QVERIFY(!outgoing->isFlushPending());
    QCOMPARE(pipeA.consumer().ready(), size_t{0});
    QCOMPARE(outgoing->clock().streamEpoch(), epoch2);
    QVERIFY(outgoing->clock().isQueueReady());

    // Post-seek samples (0.8f) arrive and fade out cleanly without any 0.25f splice.
    // The prior dry-pipe starvation quantum advanced the 1000ms fade-out by 1024 frames.
    writeAll(pipeA, makeConstant(0.8f, 1024, kTestChannels));
    outgoing->clock().samplesProduced(epoch2, 2048);
    const auto out = drainSpan(outgoing->render(2048));
    QCOMPARE(out.size(), size_t{2048});
    const float expectedFirstSample = 0.8f * std::cos((1024.0f / 44100.0f) * (std::numbers::pi_v<float> * 0.5f));
    const float noAdvanceSample = 0.8f;
    QVERIFY2(qAbs(out[0] - expectedFirstSample) < qAbs(out[0] - noAdvanceSample), "fade-out did not advance over the dry-pipe starvation quantum");
    QVERIFY(qAbs(out[0] - expectedFirstSample) < 2e-4f);
    QVERIFY(out[out.size() - 2] < out[0]);
}

void TestVoice::testPackedFlushStagingAndOverwrite()
{
    Pipe<float> pipe;
    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));

    auto primary = makeVoice(&pipe, 1);
    primary->setFormat(44100, kTestChannels);
    std::ignore = primary->publishInitialStream(0ms);

    // Stage a slow 1500ms flush, then overwrite with a 500ms flush before render()
    // consumes it; only the newer epoch and fade-in duration should apply.
    const auto ep1 = primary->stageFlush(1500ms);
    QVERIFY(primary->isFlushPending());
    const auto ep2 = primary->stageFlush(500ms);
    QVERIFY(ep2 > ep1);
    QVERIFY(primary->isFlushPending());

    QVERIFY(primary->render(2048).empty());
    QVERIFY(!primary->isFlushPending());
    QCOMPARE(primary->clock().streamEpoch(), ep2);

    // At 500ms @ 44100 Hz (= 22050 frames), 1024 stereo frames advances to
    // t = 1024 / 22050; equal-power fade-in reaches sin(t * pi / 2) (~0.0729).
    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));
    primary->clock().samplesProduced(ep2, static_cast<std::size_t>(kTestFrames) * kTestChannels);
    const auto out = drainSpan(primary->render(2048));
    QCOMPARE(out.size(), size_t{2048});
    const float expectedFrameEndGain = std::sin((1023.0f / 22050.0f) * (std::numbers::pi_v<float> * 0.5f));
    const float expectedLastSample = 0.5f * expectedFrameEndGain;
    QVERIFY2(qAbs(out.back() - expectedLastSample) < 1e-3f, "Overwritten fade-in duration must match 500ms, not 1500ms");
}

QTEST_GUILESS_MAIN(TestVoice)
#include "test_voice.moc"
