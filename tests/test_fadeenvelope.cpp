/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "mixer/fadeenvelope.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;

class TestDragonFadeEnvelope : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // FadeCurve
    void testFadeCurveDirectAndBoundaries();
    void testFadeCurveAdvanceEdgeCases();
    void testFadeCurveResetDirection();

    // FadeInEnvelope
    void testFadeInDefaultAndReset();
    void testFadeInFormatReopenEdgeCases();
    void testFadeInNegativeDuration();
    void testFormatReopen();

    // FadeOutEnvelope
    void testFadeOutRemainingBudgetFrames();
    void testFadeOutHoldConfigurationGuards();
    void testFadeOutPartialHoldAndBypass();
    void testFadeOutAdvanceChunkEdgeCases();
    void testInterruptedFadeInitialGain();
    void testSlowLoadHold();

    // Combined Envelope Math & Identities
    void testEnvelopeEndpoints();
    void testEnvelopeMonotonicity();
    void testEqualPowerIdentity();
    void testAsymmetricFadeGain();

    // applyGainRamp & mixWithGainRamp
    void testGainRampInvalidAndEmptyInputs();
    void testMixWithGainRampSpanMismatch();
    void testMixWithGainRampClamping();
    void testApplyGainRampStereoLinear();
    void testFadeSpanSIMD();
    void testFadeSpanLinearAndUnaligned();
};

void TestDragonFadeEnvelope::testFadeCurveDirectAndBoundaries()
{
    FadeCurve inCurve(FadeDirection::In);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{0});
    QCOMPARE(inCurve.positionFrames(), std::int64_t{0});
    QCOMPARE(inCurve.remainingFrames(), std::int64_t{0});
    // Resting gain for inactive fade-in is m_peakGain (1.0f)
    QCOMPARE(inCurve.currentGain(), 1.0f);

    FadeCurve outCurve(FadeDirection::Out);
    QVERIFY(!outCurve.isActive());
    // Resting gain for inactive fade-out is 0.0f
    QCOMPARE(outCurve.currentGain(), 0.0f);

    // Direct FadeCurve in Out mode: active progression and resting gain
    outCurve.configure(FadeDirection::Out, 100ms, 1000, 0.8f);
    QVERIFY(outCurve.isActive());
    QCOMPARE(outCurve.totalFrames(), std::int64_t{100});
    QCOMPARE(outCurve.positionFrames(), std::int64_t{0});
    QCOMPARE(outCurve.remainingFrames(), std::int64_t{100});
    QCOMPARE(outCurve.currentGain(), 0.8f);

    const auto outRamp40 = outCurve.advance(40);
    QCOMPARE(outRamp40.start, 0.8f);
    const float expectedOut40 = 0.8f * std::cos(0.4f * std::numbers::pi_v<float> * 0.5f);
    QVERIFY(qFuzzyCompare(outRamp40.end, expectedOut40));
    QCOMPARE(outCurve.positionFrames(), std::int64_t{40});
    QCOMPARE(outCurve.remainingFrames(), std::int64_t{60});

    const auto outRampEnd = outCurve.advance(60);
    QCOMPARE(outRampEnd.start, outRamp40.end);
    QCOMPARE(outRampEnd.end, 0.0f);
    QVERIFY(!outCurve.isActive());
    QCOMPARE(outCurve.positionFrames(), std::int64_t{100});
    QCOMPARE(outCurve.remainingFrames(), std::int64_t{0});
    QCOMPARE(outCurve.currentGain(), 0.0f);

    // Peak gain clamping in configure(): clamped into [0.0f, 1.0f]
    // In direction:
    inCurve.configure(FadeDirection::In, 100ms, 1000, 1.5f);
    std::ignore = inCurve.advance(100);
    QCOMPARE(inCurve.currentGain(), 1.0f);

    inCurve.configure(FadeDirection::In, 100ms, 1000, -0.5f);
    std::ignore = inCurve.advance(100);
    QCOMPARE(inCurve.currentGain(), 0.0f);

    // Out direction:
    outCurve.configure(FadeDirection::Out, 100ms, 1000, 1.5f);
    QCOMPARE(outCurve.currentGain(), 1.0f);

    outCurve.configure(FadeDirection::Out, 100ms, 1000, -0.5f);
    QCOMPARE(outCurve.currentGain(), 0.0f);

    // Minimum 1-frame floor when duration > 0 and sampleRate > 0:
    // 1ms at 500Hz -> (1 * 500) / 1000 = 0, but clamped to std::max(1, 0) = 1 frame.
    inCurve.configure(FadeDirection::In, 1ms, 500, 0.8f);
    QVERIFY(inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{1});
    QCOMPARE(inCurve.remainingFrames(), std::int64_t{1});
    QCOMPARE(inCurve.positionFrames(), std::int64_t{0});
    QCOMPARE(inCurve.currentGain(), 0.0f);

    // Advance 1 frame: verify {0.0f, 0.8f} and inactive resting gain is m_peakGain (0.8f)
    const auto rSingle = inCurve.advance(1);
    QCOMPARE(rSingle.start, 0.0f);
    QCOMPARE(rSingle.end, 0.8f);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.positionFrames(), std::int64_t{1});
    QCOMPARE(inCurve.remainingFrames(), std::int64_t{0});
    QCOMPARE(inCurve.currentGain(), 0.8f);

    // Zero or negative duration, or non-positive sample rate -> 0 frames (inactive)
    inCurve.configure(FadeDirection::In, 0ms, 48000);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{0});

    inCurve.configure(FadeDirection::In, -10ms, 48000);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{0});

    inCurve.configure(FadeDirection::In, 500ms, 0);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{0});

    inCurve.configure(FadeDirection::In, 500ms, -44100);
    QVERIFY(!inCurve.isActive());
    QCOMPARE(inCurve.totalFrames(), std::int64_t{0});
}

void TestDragonFadeEnvelope::testFadeCurveAdvanceEdgeCases()
{
    FadeCurve curve(FadeDirection::In);
    curve.configure(FadeDirection::In, 100ms, 1000, 1.0f);
    QCOMPARE(curve.totalFrames(), std::int64_t{100});

    // Zero or negative advance chunk at frame 0 -> flat ramp at current gain (0.0f), no movement
    const auto rZero = curve.advance(0);
    QCOMPARE(rZero.start, 0.0f);
    QCOMPARE(rZero.end, 0.0f);
    QCOMPARE(curve.positionFrames(), std::int64_t{0});

    const auto rNeg = curve.advance(-5);
    QCOMPARE(rNeg.start, 0.0f);
    QCOMPARE(rNeg.end, 0.0f);
    QCOMPARE(curve.positionFrames(), std::int64_t{0});

    const auto r40 = curve.advance(40);
    QCOMPARE(r40.start, 0.0f);
    const float expectedGain40 = std::sin(0.4f * std::numbers::pi_v<float> * 0.5f);
    QVERIFY(qFuzzyCompare(r40.end, expectedGain40));
    QVERIFY(r40.end > 0.0f && r40.end < 1.0f);
    QCOMPARE(curve.positionFrames(), std::int64_t{40});
    QCOMPARE(curve.remainingFrames(), std::int64_t{60});

    // Zero or negative advance chunk at non-zero live gain -> flat ramp at live gain, no movement
    const auto rZeroLive = curve.advance(0);
    QCOMPARE(rZeroLive.start, r40.end);
    QCOMPARE(rZeroLive.end, r40.end);
    QCOMPARE(curve.positionFrames(), std::int64_t{40});

    const auto rNegLive = curve.advance(-5);
    QCOMPARE(rNegLive.start, r40.end);
    QCOMPARE(rNegLive.end, r40.end);
    QCOMPARE(curve.positionFrames(), std::int64_t{40});

    // Advance beyond remaining (chunk = 150 > 60 remaining)
    // Should clamp position to totalFrames (100) and mark inactive
    const auto rOvershoot = curve.advance(150);
    QCOMPARE(rOvershoot.start, r40.end);
    QCOMPARE(rOvershoot.end, 1.0f);
    QCOMPARE(curve.positionFrames(), std::int64_t{100});
    QCOMPARE(curve.remainingFrames(), std::int64_t{0});
    QVERIFY(!curve.isActive());

    // Subsequent advance while inactive returns flat ramp at resting gain
    const auto rPost = curve.advance(10);
    QCOMPARE(rPost.start, 1.0f);
    QCOMPARE(rPost.end, 1.0f);
    QCOMPARE(curve.positionFrames(), std::int64_t{100});
}

void TestDragonFadeEnvelope::testFadeCurveResetDirection()
{
    FadeCurve curve(FadeDirection::In);
    curve.configure(FadeDirection::In, 100ms, 1000, 0.5f);
    std::ignore = curve.advance(50);
    QVERIFY(curve.isActive());

    // Reset with Direction::Out restores default peak gain 1.0 and resets positions
    curve.reset(FadeDirection::Out);
    QVERIFY(!curve.isActive());
    QCOMPARE(curve.positionFrames(), std::int64_t{0});
    QCOMPARE(curve.totalFrames(), std::int64_t{0});
    // Resting gain for Out is 0.0f
    QCOMPARE(curve.currentGain(), 0.0f);

    // Reset with Direction::In has resting gain 1.0f
    curve.reset(FadeDirection::In);
    QVERIFY(!curve.isActive());
    QCOMPARE(curve.currentGain(), 1.0f);
}

void TestDragonFadeEnvelope::testFadeInDefaultAndReset()
{
    FadeInEnvelope fadeIn;
    QVERIFY(!fadeIn.isActive());
    QVERIFY(!fadeIn.isWaitingForFirstSample());
    QCOMPARE(fadeIn.totalFrames(), std::int64_t{0});
    QCOMPARE(fadeIn.positionFrames(), std::int64_t{0});
    QCOMPARE(fadeIn.currentGain(), 1.0f);

    // advance on inactive envelope returns flat 1.0
    const auto ramp = fadeIn.advanceChunk(64);
    QCOMPARE(ramp.start, 1.0f);
    QCOMPARE(ramp.end, 1.0f);

    // Configure and then reset
    fadeIn.configure(200ms, 48000);
    QVERIFY(fadeIn.isActive());
    QVERIFY(fadeIn.isWaitingForFirstSample());
    fadeIn.reset();
    QVERIFY(!fadeIn.isActive());
    QVERIFY(!fadeIn.isWaitingForFirstSample());
    QCOMPARE(fadeIn.totalFrames(), std::int64_t{0});
    QCOMPARE(fadeIn.currentGain(), 1.0f);
}

void TestDragonFadeEnvelope::testFadeInFormatReopenEdgeCases()
{
    FadeInEnvelope fadeIn;
    // Configured with valid duration but sampleRate == 0 (device closed)
    fadeIn.configure(500ms, 0);
    QVERIFY(!fadeIn.isActive());
    QVERIFY(!fadeIn.isWaitingForFirstSample());

    // reset() clears the stored m_duration
    fadeIn.reset();
    // Reopening format now must NOT activate because m_duration was cleared
    fadeIn.onFormatReopen(44100);
    QVERIFY(!fadeIn.isActive());

    // Configure again while device is closed
    fadeIn.configure(300ms, 0);
    // Invalid reopened sample rate (<= 0) resets the envelope
    fadeIn.onFormatReopen(0);
    QVERIFY(!fadeIn.isActive());
    // Subsequent valid reopen does nothing because it was reset
    fadeIn.onFormatReopen(44100);
    QVERIFY(!fadeIn.isActive());
}

void TestDragonFadeEnvelope::testFadeInNegativeDuration()
{
    FadeInEnvelope fadeIn;
    fadeIn.configure(-100ms, 48000);
    QVERIFY(!fadeIn.isActive());
    QCOMPARE(fadeIn.totalFrames(), std::int64_t{0});
    QCOMPARE(fadeIn.currentGain(), 1.0f);
}

void TestDragonFadeEnvelope::testFormatReopen()
{
    FadeInEnvelope fadeIn;
    // Configured while device is closed (sampleRate == 0): inactive until reopened.
    fadeIn.configure(500ms, 0);
    QVERIFY(!fadeIn.isActive());
    QCOMPARE(fadeIn.positionFrames(), std::int64_t{0});

    fadeIn.onFormatReopen(48000);
    QVERIFY(fadeIn.isActive());
    QVERIFY(fadeIn.isWaitingForFirstSample());
    QCOMPARE(fadeIn.totalFrames(), std::int64_t{24000});

    // Reopening mid-fade tears down the envelope.
    std::ignore = fadeIn.advanceChunk(1000);
    QVERIFY(!fadeIn.isWaitingForFirstSample());
    fadeIn.onFormatReopen(44100);
    QVERIFY(!fadeIn.isActive());
    QCOMPARE(fadeIn.currentGain(), 1.0f);
}

void TestDragonFadeEnvelope::testFadeOutRemainingBudgetFrames()
{
    FadeOutEnvelope fadeOut;
    // When inactive, remainingBudgetFrames returns 0
    QCOMPARE(fadeOut.remainingBudgetFrames(false), std::size_t{0});
    QCOMPARE(fadeOut.remainingBudgetFrames(true), std::size_t{0});

    // 1000ms fade, 200ms hold at 1000Hz -> 1000 curve frames, 200 hold frames
    fadeOut.configure({.duration = 1000ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    QVERIFY(fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{200});
    QCOMPARE(fadeOut.remainingBudgetFrames(false), std::size_t{1000});
    QCOMPARE(fadeOut.remainingBudgetFrames(true), std::size_t{1200});

    // Advance 100 frames during hold
    const auto rHold = fadeOut.advanceChunk(100, true);
    QCOMPARE(rHold.start, 1.0f);
    QCOMPARE(rHold.end, 1.0f);
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{100});
    QCOMPARE(fadeOut.remainingBudgetFrames(false), std::size_t{1000});
    QCOMPARE(fadeOut.remainingBudgetFrames(true), std::size_t{1100});

    // Reset clears everything
    fadeOut.reset();
    QVERIFY(!fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});
    QCOMPARE(fadeOut.remainingBudgetFrames(true), std::size_t{0});
}

void TestDragonFadeEnvelope::testFadeOutHoldConfigurationGuards()
{
    FadeOutEnvelope fadeOut;

    // Helper lambda to setup envelope with non-zero holdRemainingFrames
    auto setupHold = [&fadeOut]() {
        fadeOut.configure({.duration = 500ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});
        QVERIFY(fadeOut.isActive());
        QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{200});
    };

    // holdMax > 0 but incomingHasFadeIn == false -> holdRemainingFrames resets to 0
    setupHold();
    fadeOut.configure({.duration = 500ms, .holdMax = 200ms, .incomingHasFadeIn = false, .sampleRate = 1000});
    QVERIFY(fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    // holdMax == 0 with incomingHasFadeIn == true -> holdRemainingFrames resets to 0
    setupHold();
    fadeOut.configure({.duration = 500ms, .holdMax = 0ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    QVERIFY(fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    // holdMax < 0 (-50ms) with incomingHasFadeIn == true -> holdRemainingFrames resets to 0
    setupHold();
    fadeOut.configure({.duration = 500ms, .holdMax = -50ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    QVERIFY(fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    // duration == 0 (inactive curve) with incomingHasFadeIn == true and holdMax > 0 -> resets to 0
    setupHold();
    fadeOut.configure({.duration = 0ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    QVERIFY(!fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    // sampleRate <= 0 (0 and -1000) -> resets to 0
    setupHold();
    fadeOut.configure({.duration = 500ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 0});
    QVERIFY(!fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    setupHold();
    fadeOut.configure({.duration = 500ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = -1000});
    QVERIFY(!fadeOut.isActive());
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});
}

void TestDragonFadeEnvelope::testFadeOutPartialHoldAndBypass()
{
    // Hold remaining frames is 200.
    FadeOutEnvelope fadeOut;
    fadeOut.configure({.duration = 1000ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});

    // Advance 75 frames held twice -> 150 frames consumed, 50 hold frames remaining
    std::ignore = fadeOut.advanceChunk(75, /*holdEnvelope=*/true);
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{125});
    std::ignore = fadeOut.advanceChunk(75, /*holdEnvelope=*/true);
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{50});
    QCOMPARE(fadeOut.positionFrames(), std::int64_t{0});

    // Advance 100 frames with holdEnvelope=false -> bypasses hold and advances curve directly
    const auto rBypass = fadeOut.advanceChunk(100, /*holdEnvelope=*/false);
    QVERIFY(rBypass.end < rBypass.start);
    QCOMPARE(fadeOut.positionFrames(), std::int64_t{100});
    // Hold frames should remain intact since hold wasn't requested
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{50});
}

void TestDragonFadeEnvelope::testFadeOutAdvanceChunkEdgeCases()
{
    // advanceChunk(0, true) and advanceChunk(-5, true) while holdRemainingFrames() > 0
    // Verifies chunkFrames <= 0 early return does not modify m_holdRemainingFrames
    FadeOutEnvelope fadeOut;
    fadeOut.configure({.duration = 1000ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{200});

    const auto rZero = fadeOut.advanceChunk(0, /*holdEnvelope=*/true);
    QCOMPARE(rZero.start, 1.0f);
    QCOMPARE(rZero.end, 1.0f);
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{200});

    const auto rNeg = fadeOut.advanceChunk(-5, /*holdEnvelope=*/true);
    QCOMPARE(rNeg.start, 1.0f);
    QCOMPARE(rNeg.end, 1.0f);
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{200});

    // Holding when currentGain() != 1.0f:
    // With initialGain = 0.6f
    FadeOutEnvelope fadeOutInitial;
    fadeOutInitial.configure({.duration = 1000ms, .holdMax = 200ms, .initialGain = 0.6f, .incomingHasFadeIn = true, .sampleRate = 1000});
    QCOMPARE(fadeOutInitial.currentGain(), 0.6f);
    const auto rHoldInit = fadeOutInitial.advanceChunk(50, /*holdEnvelope=*/true);
    QCOMPARE(rHoldInit.start, 0.6f);
    QCOMPARE(rHoldInit.end, 0.6f);
    QCOMPARE(fadeOutInitial.holdRemainingFrames(), std::int64_t{150});

    // 2b. Holding after curve advance (currentGain < 1.0f)
    FadeOutEnvelope fadeOutMid;
    fadeOutMid.configure({.duration = 1000ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    const auto rBypass = fadeOutMid.advanceChunk(100, /*holdEnvelope=*/false);
    QVERIFY(rBypass.end < rBypass.start);
    const float gainAfterBypass = fadeOutMid.currentGain();
    QVERIFY(gainAfterBypass < 1.0f);
    QCOMPARE(fadeOutMid.holdRemainingFrames(), std::int64_t{200});

    // Now hold: must return flat ramp at gainAfterBypass, not 1.0f
    const auto rHoldAfterBypass = fadeOutMid.advanceChunk(50, /*holdEnvelope=*/true);
    QCOMPARE(rHoldAfterBypass.start, gainAfterBypass);
    QCOMPARE(rHoldAfterBypass.end, gainAfterBypass);
    QCOMPARE(fadeOutMid.holdRemainingFrames(), std::int64_t{150});
    QCOMPARE(fadeOutMid.positionFrames(), std::int64_t{100});
}

void TestDragonFadeEnvelope::testInterruptedFadeInitialGain()
{
    // Starting a crossfade while the primary channel is mid-fade-in scales
    // the fade-out by the outgoing track's actual audible gain instead of
    // jumping up to 1.0.
    FadeOutEnvelope fadeOut;
    fadeOut.configure({.duration = 1000ms, .initialGain = 0.6f, .sampleRate = 1000});

    QVERIFY(qFuzzyCompare(fadeOut.currentGain(), 0.6f));
    const auto ramp = fadeOut.advanceChunk(500);
    QVERIFY(qFuzzyCompare(ramp.start, 0.6f));
    QVERIFY(ramp.end < ramp.start);
    QVERIFY(ramp.end > 0.0f);
    std::ignore = fadeOut.advanceChunk(500);
    QVERIFY(qAbs(fadeOut.currentGain()) < 1e-4f);

    // Out-of-bounds initial gains are clamped into [0.0, 1.0].
    fadeOut.configure({.duration = 1000ms, .initialGain = 1.5f, .sampleRate = 1000});
    QCOMPARE(fadeOut.currentGain(), 1.0f);
    fadeOut.configure({.duration = 1000ms, .initialGain = -0.25f, .sampleRate = 1000});
    QCOMPARE(fadeOut.currentGain(), 0.0f);
}

void TestDragonFadeEnvelope::testSlowLoadHold()
{
    // While the incoming track is still loading, the outgoing envelope
    // holds at its current gain for up to the hold budget.
    FadeOutEnvelope fadeOut;
    fadeOut.configure({.duration = 1000ms, .holdMax = 200ms, .incomingHasFadeIn = true, .sampleRate = 1000});

    const auto ramp = fadeOut.advanceChunk(200, /*holdEnvelope=*/true);
    QVERIFY(qFuzzyCompare(ramp.start, 1.0f));
    QVERIFY(qFuzzyCompare(ramp.end, 1.0f));
    QCOMPARE(fadeOut.positionFrames(), std::int64_t{0});
    QCOMPARE(fadeOut.holdRemainingFrames(), std::int64_t{0});

    // Hold budget exhausted: the envelope advances again.
    const auto ramp2 = fadeOut.advanceChunk(100, /*holdEnvelope=*/true);
    QVERIFY(ramp2.end < ramp2.start);
    QCOMPARE(fadeOut.positionFrames(), std::int64_t{100});

    // Chunk-boundary quantization: if hold budget is smaller than chunk size,
    // clear hold and begin ramping smoothly across the chunk.
    FadeOutEnvelope fadeOut2;
    fadeOut2.configure({.duration = 1000ms, .holdMax = 50ms, .incomingHasFadeIn = true, .sampleRate = 1000});

    const auto ramp3 = fadeOut2.advanceChunk(100, /*holdEnvelope=*/true);
    QCOMPARE(fadeOut2.holdRemainingFrames(), std::int64_t{0});
    QCOMPARE(fadeOut2.positionFrames(), std::int64_t{100});
    QVERIFY(ramp3.end < ramp3.start);
}

void TestDragonFadeEnvelope::testEnvelopeEndpoints()
{
    FadeOutEnvelope fadeOut;
    FadeInEnvelope fadeIn;
    fadeOut.configure({.duration = 1000ms, .sampleRate = 1000});
    fadeIn.configure(1000ms, 1000);

    QVERIFY(qFuzzyCompare(fadeOut.currentGain(), 1.0f));
    QVERIFY(qFuzzyCompare(fadeIn.currentGain(), 0.0f));

    const auto outRamp = fadeOut.advanceChunk(1000);
    const auto inRamp = fadeIn.advanceChunk(1000);
    QVERIFY(qFuzzyCompare(outRamp.start, 1.0f));
    QCOMPARE(outRamp.end, 0.0f);
    QCOMPARE(inRamp.start, 0.0f);
    QVERIFY(qFuzzyCompare(inRamp.end, 1.0f));

    QVERIFY(!fadeOut.isActive());
    QVERIFY(!fadeIn.isActive());
    QCOMPARE(fadeOut.currentGain(), 0.0f);
    QVERIFY(qFuzzyCompare(fadeIn.currentGain(), 1.0f));
}

void TestDragonFadeEnvelope::testEnvelopeMonotonicity()
{
    FadeOutEnvelope fadeOut;
    FadeInEnvelope fadeIn;
    fadeOut.configure({.duration = 1000ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    fadeIn.configure(1000ms, 1000);

    float lastOut = std::numeric_limits<float>::max();
    float lastIn = std::numeric_limits<float>::lowest();
    for (int chunk = 0; chunk < 20; ++chunk) {
        const auto outRamp = fadeOut.advanceChunk(50);
        const auto inRamp = fadeIn.advanceChunk(50);
        QVERIFY(outRamp.start <= lastOut);
        QVERIFY(outRamp.end <= outRamp.start);
        QVERIFY(inRamp.start >= lastIn);
        QVERIFY(inRamp.end >= inRamp.start);
        lastOut = outRamp.end;
        lastIn = inRamp.end;
    }
    QVERIFY(qAbs(lastOut) < 1e-4f);
    QVERIFY(qAbs(lastIn - 1.0f) < 1e-4f);
}

void TestDragonFadeEnvelope::testEqualPowerIdentity()
{
    // For symmetric durations starting from unity gain, the equal-power
    // curves must sum to constant power 1.0 at every chunk endpoint.
    FadeOutEnvelope fadeOut;
    FadeInEnvelope fadeIn;
    fadeOut.configure({.duration = 100ms, .incomingHasFadeIn = true, .sampleRate = 44100});
    fadeIn.configure(100ms, 44100);
    QCOMPARE(fadeOut.totalFrames(), std::int64_t{4410});
    QCOMPARE(fadeIn.totalFrames(), std::int64_t{4410});

    for (int step = 0; step <= 21; ++step) {
        const float out = fadeOut.currentGain();
        const float in = fadeIn.currentGain();
        QVERIFY(qAbs(out * out + in * in - 1.0f) < 1e-4f);
        if (step < 21) {
            std::ignore = fadeOut.advanceChunk(210);
            std::ignore = fadeIn.advanceChunk(210);
        }
    }
}

void TestDragonFadeEnvelope::testAsymmetricFadeGain()
{
    // Asymmetric durations: each stream follows its own curve; no power
    // identity is claimed, but endpoints and monotonicity still hold.
    FadeOutEnvelope fadeOut;
    FadeInEnvelope fadeIn;
    fadeOut.configure({.duration = 1000ms, .incomingHasFadeIn = true, .sampleRate = 1000});
    fadeIn.configure(4000ms, 1000);

    for (int chunk = 0; chunk < 10; ++chunk) {
        std::ignore = fadeOut.advanceChunk(100);
        std::ignore = fadeIn.advanceChunk(400);
    }
    QVERIFY(!fadeOut.isActive());
    QVERIFY(qFuzzyCompare(fadeIn.currentGain(), 1.0f));
}

void TestDragonFadeEnvelope::testGainRampInvalidAndEmptyInputs()
{
    std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f};
    const auto copy = data;

    // channels <= 0 is a no-op (test both flat and non-flat ramps)
    applyGainRamp(data, 0, {0.0f, 0.0f});
    QCOMPARE(data, copy);
    applyGainRamp(data, -1, {0.0f, 0.0f});
    QCOMPARE(data, copy);
    applyGainRamp(data, 0, {0.0f, 1.0f});
    QCOMPARE(data, copy);
    applyGainRamp(data, -1, {0.0f, 1.0f});
    QCOMPARE(data, copy);

    // empty dst is a no-op
    std::span<float> emptyDst;
    applyGainRamp(emptyDst, 2, {0.0f, 0.0f});
    applyGainRamp(emptyDst, 2, {0.0f, 1.0f});

    // mixWithGainRamp with invalid channels (flat and non-flat ramps)
    std::vector<float> src = {0.5f, 0.5f, 0.5f, 0.5f};
    mixWithGainRamp(data, src, 0, {1.0f, 1.0f});
    QCOMPARE(data, copy);
    mixWithGainRamp(data, src, -2, {1.0f, 1.0f});
    QCOMPARE(data, copy);
    mixWithGainRamp(data, src, 0, {0.0f, 1.0f});
    QCOMPARE(data, copy);
    mixWithGainRamp(data, src, -2, {0.0f, 1.0f});
    QCOMPARE(data, copy);

    // mixWithGainRamp with empty spans
    std::span<const float> emptySrc;
    mixWithGainRamp(data, emptySrc, 2, {1.0f, 1.0f});
    QCOMPARE(data, copy);
    mixWithGainRamp(data, emptySrc, 2, {0.0f, 1.0f});
    QCOMPARE(data, copy);

    std::vector<float> emptyDstVec;
    mixWithGainRamp(emptyDstVec, src, 2, {1.0f, 1.0f});
    QVERIFY(emptyDstVec.empty());
    mixWithGainRamp(emptyDstVec, src, 2, {0.0f, 1.0f});
    QVERIFY(emptyDstVec.empty());

    mixWithGainRamp(emptyDstVec, emptySrc, 2, {0.0f, 1.0f});
    QVERIFY(emptyDstVec.empty());
}

void TestDragonFadeEnvelope::testMixWithGainRampSpanMismatch()
{
    // Fast-path {1.0f, 1.0f}:
    // src smaller than dst: only first src.size() samples modified
    std::vector<float> dst = {0.1f, 0.1f, 0.1f, 0.1f, 0.9f, 0.9f};
    const std::vector<float> src = {0.2f, 0.2f};
    mixWithGainRamp(dst, src, 2, {1.0f, 1.0f});

    QVERIFY(qFuzzyCompare(dst[0], 0.3f));
    QVERIFY(qFuzzyCompare(dst[1], 0.3f));
    // Unmodified elements beyond src.size()
    QCOMPARE(dst[2], 0.1f);
    QCOMPARE(dst[3], 0.1f);
    QCOMPARE(dst[4], 0.9f);
    QCOMPARE(dst[5], 0.9f);

    // dst smaller than src: safely mixes dst.size() without reading out of bounds
    std::vector<float> dstSmall = {0.2f, 0.2f};
    const std::vector<float> srcLarge = {0.3f, 0.3f, 10.0f, 10.0f};
    mixWithGainRamp(dstSmall, srcLarge, 2, {1.0f, 1.0f});
    QVERIFY(qFuzzyCompare(dstSmall[0], 0.5f));
    QVERIFY(qFuzzyCompare(dstSmall[1], 0.5f));

    // Ramped path {0.2f, 0.8f} with mismatched spans:
    // src smaller than dst: sampleCount = min(6, 4) = 4 (2 stereo frames)
    // gainStep computed from sampleCount (frames = 2), not dst.size() (frames = 3)
    std::vector<float> dstRamped = {0.1f, 0.2f, 0.3f, 0.4f, 0.9f, 0.9f};
    const std::vector<float> srcRamped = {0.5f, 0.5f, 0.5f, 0.5f};
    mixWithGainRamp(dstRamped, srcRamped, 2, {0.2f, 0.8f});
    // frames = 2, gainStep = (0.8 - 0.2) / 2 = 0.3f.
    // Frame 0: g = 0.2 -> dst[0] = 0.1 + 0.5*0.2 = 0.2f, dst[1] = 0.2 + 0.5*0.2 = 0.3f
    // Frame 1: g = 0.5 -> dst[2] = 0.3 + 0.5*0.5 = 0.55f, dst[3] = 0.4 + 0.5*0.5 = 0.65f
    // Tail untouched: dst[4], dst[5]
    QVERIFY(qFuzzyCompare(dstRamped[0], 0.2f));
    QVERIFY(qFuzzyCompare(dstRamped[1], 0.3f));
    QVERIFY(qFuzzyCompare(dstRamped[2], 0.55f));
    QVERIFY(qFuzzyCompare(dstRamped[3], 0.65f));
    QCOMPARE(dstRamped[4], 0.9f);
    QCOMPARE(dstRamped[5], 0.9f);

    // dst smaller than src: sampleCount = min(2, 6) = 2 (1 stereo frame)
    std::vector<float> dstSmallRamped = {0.1f, 0.2f};
    const std::vector<float> srcLargeRamped = {0.5f, 0.5f, 10.0f, 10.0f, 20.0f, 20.0f};
    mixWithGainRamp(dstSmallRamped, srcLargeRamped, 2, {0.4f, 0.8f});
    // frames = 1, gainStep = 0.4, Frame 0: g = 0.4 -> dst[0] = 0.1 + 0.5*0.4 = 0.3f, dst[1] = 0.2 + 0.5*0.4 = 0.4f
    QVERIFY(qFuzzyCompare(dstSmallRamped[0], 0.3f));
    QVERIFY(qFuzzyCompare(dstSmallRamped[1], 0.4f));
}

void TestDragonFadeEnvelope::testMixWithGainRampClamping()
{
    // Constant unity {1.0f, 1.0f} clamping to [-1.0f, 1.0f]
    std::vector<float> dstPos = {0.8f, -0.8f};
    std::vector<float> srcPos = {0.5f, -0.5f};
    mixWithGainRamp(dstPos, srcPos, 2, {1.0f, 1.0f});
    QCOMPARE(dstPos[0], 1.0f);
    QCOMPARE(dstPos[1], -1.0f);

    // Constant non-unity {0.5f, 0.5f} clamping
    std::vector<float> dstConst = {0.7f, -0.7f};
    std::vector<float> srcConst = {0.8f, -0.8f}; // 0.7 + 0.8 * 0.5 = 1.1 -> 1.0f
    mixWithGainRamp(dstConst, srcConst, 2, {0.5f, 0.5f});
    QCOMPARE(dstConst[0], 1.0f);
    QCOMPARE(dstConst[1], -1.0f);

    // Stereo ramping {0.0f, 1.0f} clamping with positive and negative clamping lanes
    std::vector<float> dstStereo = {0.8f, -0.8f, 0.8f, -0.8f};
    std::vector<float> srcStereo = {0.5f, -0.5f, 0.5f, -0.5f};
    mixWithGainRamp(dstStereo, srcStereo, 2, {0.0f, 1.0f});
    // Frame 0: g=0.0 -> dst unmodified
    // Frame 1: g=0.5 -> 0.8 + 0.25 = 1.05 -> clamped to 1.0f; -0.8 - 0.25 = -1.05 -> clamped to -1.0f
    QVERIFY(qFuzzyCompare(dstStereo[0], 0.8f));
    QVERIFY(qFuzzyCompare(dstStereo[1], -0.8f));
    QCOMPARE(dstStereo[2], 1.0f);
    QCOMPARE(dstStereo[3], -1.0f);

    // Multichannel (channels=3) ramping and trailing remainder clamping
    // 5 samples: 1 full 3-channel frame + 2 trailing remainder samples
    std::vector<float> dstMulti = {0.8f, -0.8f, 0.5f, 0.8f, -0.8f};
    std::vector<float> srcMulti = {0.5f, -0.5f, 0.5f, 0.5f, -0.5f};
    mixWithGainRamp(dstMulti, srcMulti, 3, {0.5f, 1.0f});
    // Frame 0 (f=0, g=0.5):
    // ch 0: 0.8 + 0.5 * 0.5 = 1.05 -> clamped to 1.0f
    // ch 1: -0.8 + (-0.5) * 0.5 = -1.05 -> clamped to -1.0f
    // ch 2: 0.5 + 0.5 * 0.5 = 0.75f (unclamped)
    // Trailing remainder (indices 3, 4 with ramp.end = 1.0f):
    // idx 3: 0.8 + 0.5 * 1.0 = 1.3 -> clamped to 1.0f
    // idx 4: -0.8 + (-0.5) * 1.0 = -1.3 -> clamped to -1.0f
    QCOMPARE(dstMulti[0], 1.0f);
    QCOMPARE(dstMulti[1], -1.0f);
    QVERIFY(qFuzzyCompare(dstMulti[2], 0.75f));
    QCOMPARE(dstMulti[3], 1.0f);
    QCOMPARE(dstMulti[4], -1.0f);
}

void TestDragonFadeEnvelope::testApplyGainRampStereoLinear()
{
    // Dedicated unit test for stereo hot loop (channels == 2, ramp.start != ramp.end, frames > 0)
    // and trailing remainder scaling.
    std::vector<float> stereo = {1.0f, 2.0f, 1.0f, 2.0f, 1.0f}; // 2 stereo frames + 1 trailing sample
    applyGainRamp(stereo, 2, {0.2f, 0.8f});
    // frames = 2, gainStep = (0.8 - 0.2) / 2 = 0.3f.
    // Frame 0: g = 0.2 -> stereo[0] = 0.2f, stereo[1] = 0.4f
    // Frame 1: g = 0.5 -> stereo[2] = 0.5f, stereo[3] = 1.0f
    // Trailing sample (idx 4): scaled by ramp.end (0.8f) -> 0.8f
    QVERIFY(qFuzzyCompare(stereo[0], 0.2f));
    QVERIFY(qFuzzyCompare(stereo[1], 0.4f));
    QVERIFY(qFuzzyCompare(stereo[2], 0.5f));
    QVERIFY(qFuzzyCompare(stereo[3], 1.0f));
    QVERIFY(qFuzzyCompare(stereo[4], 0.8f));
}

void TestDragonFadeEnvelope::testFadeSpanSIMD()
{
    // applyGainRamp fast-path tests
    std::vector<float> buffer = {0.8f, -0.6f, 0.4f, -0.2f};
    const auto original = buffer;

    // Fast-path: ramp 1.0 -> 1.0 (no-op)
    applyGainRamp(buffer, 2, {1.0f, 1.0f});
    QCOMPARE(buffer, original);

    // Fast-path: ramp 0.0 -> 0.0 (zero fill)
    applyGainRamp(buffer, 2, {0.0f, 0.0f});
    for (float v : buffer) {
        QCOMPARE(v, 0.0f);
    }

    // Fast-path: flat constant gain (0.5)
    buffer = original;
    applyGainRamp(buffer, 2, {0.5f, 0.5f});
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        QVERIFY(qFuzzyCompare(buffer[i], original[i] * 0.5f));
    }

    // mixWithGainRamp fast-path tests
    std::vector<float> dst = {0.2f, -0.1f, 0.3f, -0.4f};
    const std::vector<float> src = {0.5f, 0.5f, -0.5f, -0.5f};
    const auto dstOrig = dst;

    // Fast-path: ramp 0.0 -> 0.0 (no-op)
    mixWithGainRamp(dst, src, 2, {0.0f, 0.0f});
    QCOMPARE(dst, dstOrig);

    // Fast-path: ramp 1.0 -> 1.0 (clamped vector sum)
    mixWithGainRamp(dst, src, 2, {1.0f, 1.0f});
    for (std::size_t i = 0; i < dst.size(); ++i) {
        const float expected = std::clamp(dstOrig[i] + src[i], -1.0f, 1.0f);
        QVERIFY(qFuzzyCompare(dst[i], expected));
    }

    // Fast-path: flat constant gain (0.5)
    dst = dstOrig;
    mixWithGainRamp(dst, src, 2, {0.5f, 0.5f});
    for (std::size_t i = 0; i < dst.size(); ++i) {
        const float expected = std::clamp(dstOrig[i] + src[i] * 0.5f, -1.0f, 1.0f);
        QVERIFY(qFuzzyCompare(dst[i], expected));
    }
}

void TestDragonFadeEnvelope::testFadeSpanLinearAndUnaligned()
{
    // Sub-frame span (frames == 0) scales with ramp.end instead of leaking unity samples.
    std::vector<float> subFrame = {0.8f};
    applyGainRamp(subFrame, 2, {0.25f, 0.0f});
    QCOMPARE(subFrame[0], 0.0f);

    // Multichannel (3ch) linear ramp + trailing remainder sample.
    std::vector<float> multi(7, 1.0f); // 2 full 3-channel frames + 1 trailing sample
    applyGainRamp(multi, 3, {0.2f, 0.6f});
    // Frame 0: g = 0.2, Frame 1: g = 0.4, trailing sample: ramp.end = 0.6
    for (int c = 0; c < 3; ++c) {
        QVERIFY(qFuzzyCompare(multi[static_cast<std::size_t>(c)], 0.2f));
        QVERIFY(qFuzzyCompare(multi[static_cast<std::size_t>(3 + c)], 0.4f));
    }
    QVERIFY(qFuzzyCompare(multi[6], 0.6f));

    // mixWithGainRamp on sub-frame + trailing remainder applies ramp.end cleanly.
    std::vector<float> dst(5, 0.1f); // 2 stereo frames + 1 trailing sample
    const std::vector<float> src(5, 1.0f);
    mixWithGainRamp(dst, src, 2, {0.0f, 0.5f});
    QVERIFY(qFuzzyCompare(dst[0], 0.1f));
    QVERIFY(qFuzzyCompare(dst[1], 0.1f));
    QVERIFY(qFuzzyCompare(dst[2], 0.35f));
    QVERIFY(qFuzzyCompare(dst[3], 0.35f));
    QVERIFY(qFuzzyCompare(dst[4], 0.6f));
}

QTEST_GUILESS_MAIN(TestDragonFadeEnvelope)
#include "test_fadeenvelope.moc"
