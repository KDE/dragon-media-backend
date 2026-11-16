/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "player/pipe.h"
#include "voice/voicechannel.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <span>
#include <stop_token>
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

// The fixed-pipe channel of one voice: pull with ring-wrap-safe copying,
// the equal-power fade-in, and the fade-out envelope with slow-load hold.
// All state is audio-thread confined, so the tests drive it directly on the
// test thread like a backend callback would.
class TestVoiceChannel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPrimaryInstallAndFadeEndpoints();
    void testPrimaryCurrentGainMidFadeMatchesLastSampleGain();
    void testFadingSlowLoadHoldTransitionsToCosineDecay();
    void testFadingRingBufferWrapSeamContinuity();
    void testCrossfadeGainMirrorMatchesPartiallyFadedInGain();
};

void TestVoiceChannel::testPrimaryInstallAndFadeEndpoints()
{
    Pipe<float> pipe;
    VoiceChannel channel(&pipe);
    writeAll(pipe, makeConstant(1.0f, 1000, 2));

    channel.configureFadeIn(1000ms, 44100);
    QVERIFY(channel.isFadeInActive());
    QVERIFY(channel.isWaitingForFirstSample());
    QCOMPARE(channel.currentGain(), 0.0f);

    std::vector<float> buf(2000, 0.0f);

    // An empty applyFade does not release the slow-load waiting flag.
    channel.applyFadeIn(buf, 0, 2);
    QVERIFY(channel.isWaitingForFirstSample());

    const size_t read = channel.pull(buf, 200);
    QCOMPARE(read, size_t{200});
    channel.applyFadeIn(buf, read, 2);

    QVERIFY(!channel.isWaitingForFirstSample());
    QVERIFY(channel.isFadeInActive());
    QVERIFY(channel.currentGain() > 0.0f);
}

void TestVoiceChannel::testPrimaryCurrentGainMidFadeMatchesLastSampleGain()
{
    Pipe<float> pipe;
    VoiceChannel channel(&pipe);
    const int totalFrames = 1000;
    const int sampleRate = 1000;
    writeAll(pipe, makeConstant(1.0f, totalFrames, 2));

    channel.configureFadeIn(1000ms, sampleRate);

    std::vector<float> buf(600, 0.0f);
    const size_t read1 = channel.pull(buf, 600);
    QCOMPARE(read1, size_t{600});
    channel.applyFadeIn(buf, read1, 2);

    constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
    const float expectedGain300 = std::sin((300.0f / 1000.0f) * kHalfPi);

    QVERIFY(qAbs(channel.currentGain() - expectedGain300) < 1e-5f);
    const float expectedSampleGain299 = (299.0f / 300.0f) * expectedGain300;
    QVERIFY(qAbs(buf[598] - expectedSampleGain299) < 1e-4f);
    QVERIFY(qAbs(buf[599] - expectedSampleGain299) < 1e-4f);

    std::vector<float> bufRest(1400, 0.0f);
    const size_t read2 = channel.pull(bufRest, 1400);
    QCOMPARE(read2, size_t{1400});
    channel.applyFadeIn(bufRest, read2, 2);

    // First frame of chunk 2 starts at the exact gain where chunk 1 ended.
    QVERIFY(qAbs(bufRest[0] - expectedGain300) < 1e-4f);
    QVERIFY(qAbs(bufRest[1] - expectedGain300) < 1e-4f);

    QVERIFY(!channel.isFadeInActive());
    QCOMPARE(channel.currentGain(), 1.0f);
    const float expectedLastSampleGain = expectedGain300 + (699.0f / 700.0f) * (1.0f - expectedGain300);
    QVERIFY(qAbs(bufRest[1398] - expectedLastSampleGain) < 1e-4f);
    QVERIFY(qAbs(bufRest[1399] - expectedLastSampleGain) < 1e-4f);
}

void TestVoiceChannel::testFadingSlowLoadHoldTransitionsToCosineDecay()
{
    Pipe<float> pipe;
    VoiceChannel channel(&pipe);
    const int sampleRate = 1000;
    const int channels = 2;
    writeAll(pipe, makeConstant(1.0f, 1000, channels));

    channel.configureFadeOut(
        FadeOutEnvelope::Config{.duration = 1000ms, .holdMax = 500ms, .initialGain = 1.0f, .incomingHasFadeIn = true, .sampleRate = sampleRate});
    QVERIFY(channel.isFadeOutActive());

    // Chunk 1: 200 frames with holdForSlowLoad = true (gain held at 1.0f).
    std::vector<float> buf1(400, 0.0f);
    const std::size_t budget1 = channel.fadeOutBudgetSamples(buf1.size(), channels, true);
    QCOMPARE(budget1, std::size_t{400});
    const size_t read1 = channel.pull(buf1, budget1);
    QCOMPARE(read1, size_t{400});
    channel.applyFadeOut(buf1, read1, channels, true);
    QVERIFY(qAbs(buf1[0] - 1.0f) < 1e-4f);
    QVERIFY(qAbs(buf1[398] - 1.0f) < 1e-4f);

    // Chunk 2: 400 frames with holdForSlowLoad = true. 300 frames of hold
    // remain, then the cosine decay starts.
    std::vector<float> buf2(800, 0.0f);
    const std::size_t budget2 = channel.fadeOutBudgetSamples(buf2.size(), channels, true);
    QCOMPARE(budget2, std::size_t{800});
    const size_t read2 = channel.pull(buf2, budget2);
    QCOMPARE(read2, size_t{800});
    channel.applyFadeOut(buf2, read2, channels, true);
    QVERIFY(buf2[798] < 1.0f);

    // The envelope advanced over real quanta: only the un-advanced fade
    // frames remain once the hold is not counted.
    QVERIFY(channel.fadeOutBudgetSamples(2000, channels, false) < 2000);
}

void TestVoiceChannel::testFadingRingBufferWrapSeamContinuity()
{
    Pipe<float> pipe;
    VoiceChannel channel(&pipe);

    // Push the pipe near the 65536-sample ring boundary.
    writeAll(pipe, makeConstant(0.0f, 32000, 2));
    pipe.consumer().readSomeWith(64000, [](auto, auto) { });
    // Write 3000 stereo frames of 1.0f crossing the boundary.
    writeAll(pipe, makeConstant(1.0f, 3000, 2));

    channel.configureFadeOut(FadeOutEnvelope::Config{.duration = 3000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 1000});

    std::vector<float> buf(2646, 0.0f);
    const size_t read = channel.pull(buf, 2646);
    QCOMPARE(read, size_t{2646});
    channel.applyFadeOut(buf, read, 2, false);

    // Monotonic decay across the whole contiguous chunk: pull() copies
    // ring-wrap segments into one span, so the b1/b2 seam no longer exists
    // for the in-place gain ramp.
    for (size_t f = 1; f < read / 2; ++f) {
        QVERIFY(buf[2 * f] <= buf[2 * (f - 1)] + 1e-6f);
        QVERIFY(buf[2 * f + 1] <= buf[2 * (f - 1) + 1] + 1e-6f);
    }

    // Also verify a 6-channel (5.1) wrap seam where 65536 % 6 == 4 splits a
    // frame across the ring segments.
    Pipe<float> pipe6;
    VoiceChannel channel6(&pipe6);
    writeAll(pipe6, makeConstant(0.0f, 10900, 6));
    pipe6.consumer().readSomeWith(65400, [](auto, auto) { });
    writeAll(pipe6, makeConstant(1.0f, 500, 6));

    channel6.configureFadeOut(FadeOutEnvelope::Config{.duration = 1000ms, .holdMax = 0ms, .initialGain = 1.0f, .incomingHasFadeIn = false, .sampleRate = 1000});

    std::vector<float> buf6(600, 0.0f);
    const size_t read6 = channel6.pull(buf6, 600);
    QCOMPARE(read6, size_t{600});
    channel6.applyFadeOut(buf6, read6, 6, false);
    for (size_t f = 0; f < read6 / 6; ++f) {
        for (size_t c = 1; c < 6; ++c) {
            QVERIFY(qFuzzyCompare(buf6[6 * f + c], buf6[6 * f]));
        }
        if (f > 0) {
            QVERIFY(buf6[6 * f] <= buf6[6 * (f - 1)] + 1e-6f);
        }
    }
}

void TestVoiceChannel::testCrossfadeGainMirrorMatchesPartiallyFadedInGain()
{
    // The channel mirrors its current gain atomically so the Qt main thread
    // can arm a crossfade out of a partially faded-in voice without touching
    // audio-thread state.
    Pipe<float> pipe;
    VoiceChannel channel(&pipe);
    const int sampleRate = 1000;
    writeAll(pipe, makeConstant(1.0f, 1000, 2));

    channel.configureFadeIn(1000ms, sampleRate);
    QCOMPARE(channel.currentGainForCrossfade(), 0.0f);

    std::vector<float> buf(400, 0.0f);
    const size_t read = channel.pull(buf, 400);
    channel.applyFadeIn(buf, read, 2);

    const float expectedGain200 = std::sin(0.2f * (std::numbers::pi_v<float> * 0.5f));
    QVERIFY(qAbs(channel.currentGainForCrossfade() - expectedGain200) < 1e-4f);
}

QTEST_GUILESS_MAIN(TestVoiceChannel)
#include "test_voicechannel.moc"
