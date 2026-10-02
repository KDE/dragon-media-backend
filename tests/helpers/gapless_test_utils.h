/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Shared helpers for the end-to-end gapless transition tests
 * (SDL, PipeWire, PulseAudio backends).
 *
 * Provides marker-signature search in captured PCM, fixture file
 * lifetime management, and sample-level verification of the captured
 * gapless stream.
 */

#pragma once

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "test_utils.h"

#include "player/dragondiagnostics.h"
#include <DragonPlayer>

#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace GaplessTestUtils
{

inline constexpr int kFlacDrainToleranceDivisor = 10; // 100ms at sampleRate
inline constexpr double kFlacQuantTolerance = 1.5 / 32768.0;
inline constexpr int kVerifyEdgeFrames = 100;

struct MarkerHit {
    bool found = false;
    qint64 frameIndex = -1;
    float amplitude = 0.0f;
};

[[nodiscard]] inline MarkerHit
findSignature(const std::vector<float> &pcm, int channels, const std::array<float, 4> &signature, bool searchFromEnd, float threshold = 0.5f)
{
    MarkerHit hit;
    const int sigLen = static_cast<int>(signature.size());
    const int totalFrames = static_cast<int>(pcm.size()) / channels;

    if (totalFrames < sigLen) {
        return hit;
    }

    auto matchesSignature = [&](int frameIdx) -> bool {
        for (int k = 0; k < sigLen; ++k) {
            if (const float sample = pcm[(frameIdx + k) * channels]; std::abs(sample - signature[k]) > threshold) {
                return false;
            }
        }
        return true;
    };

    if (searchFromEnd) {
        for (int frm = totalFrames - sigLen; frm >= 0; --frm) {
            if (matchesSignature(frm)) {
                hit.found = true;
                hit.frameIndex = frm;
                hit.amplitude = pcm[frm * channels];
                return hit;
            }
        }
    } else {
        for (int frm = 0; frm <= totalFrames - sigLen; ++frm) {
            if (matchesSignature(frm)) {
                hit.found = true;
                hit.frameIndex = frm;
                hit.amplitude = pcm[frm * channels];
                return hit;
            }
        }
    }

    return hit;
}

struct FixtureGuard {
    QString path;
    ~FixtureGuard()
    {
        QFile::remove(path);
    }
};

[[nodiscard]] inline bool verifySamples(const std::vector<float> &captured,
                                        int captureStartFrame,
                                        const std::vector<float> &expected,
                                        int expectedStartFrame,
                                        int frames,
                                        int channels,
                                        double tolerance,
                                        const char *label)
{
    int mismatches = 0;
    double maxDev = 0.0;
    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const double dev = std::abs(static_cast<double>(captured[(captureStartFrame + i) * channels + ch])
                                        - static_cast<double>(expected[(expectedStartFrame + i) * channels + ch]));
            maxDev = std::max(maxDev, dev);
            if (dev > tolerance) {
                ++mismatches;
            }
        }
    }
    qDebug() << label << "mismatches=" << mismatches << "/" << (frames * channels) << "maxDeviation=" << maxDev << "tolerance=" << tolerance;
    return mismatches == 0;
}

// Shared playback orchestration for same-format gapless tests: loads fixtureA,
// queues fixtureB, plays through the transition, and verifies the state machine
// never stops and no underruns occur. Returns false on the first failed check
// (QVERIFY2-style fatal failures still abort the calling test immediately).
[[nodiscard]] inline bool runGaplessPlayback(DragonPlayer &player,
                                             PlayerHelper &helper,
                                             DragonDiagnostics &diagnostics,
                                             const BoundaryFixture &fixtureA,
                                             const BoundaryFixture &fixtureB,
                                             const std::function<bool()> &afterPlaybackStarted = nullptr,
                                             int stopTimeoutMs = 15000,
                                             int drainWaitMs = 1000)
{
    if (!helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath))) {
        qWarning() << "setSourceAndWait failed for" << fixtureA.filePath;
        return false;
    }
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));

    if (!helper.playAndWait()) {
        qWarning() << "playAndWait failed";
        return false;
    }
    if (!diagnostics.isAudioActive()) {
        qWarning() << "Audio should be active after play";
        return false;
    }

    if (afterPlaybackStarted && !afterPlaybackStarted()) {
        return false;
    }

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);
    waitForPipelinePrimed(diagnostics);
    const int underrunsBefore = diagnostics.audioUnderrunCount();

    if (!helper.waitForTrackChange(30000)) {
        qWarning() << "Gapless track change did not occur within timeout";
        return false;
    }

    if (!helper.verifyNoStopState(stateSpy)) {
        qWarning() << "Playback state stopped during gapless transition";
        return false;
    }
    if (!helper.verifyNoEndOfMedia(statusSpy)) {
        qWarning() << "EndOfMedia emitted during gapless transition";
        return false;
    }

    if (!(diagnostics.isAudioActive())) {
        qWarning() << "Audio should remain active after gapless transition";
        return false;
    }
    const int underrunsAfter = diagnostics.audioUnderrunCount();
    if (underrunsAfter - underrunsBefore != 0) {
        qWarning() << "Audio underrun during gapless transition: before=" << underrunsBefore << "after=" << underrunsAfter;
        return false;
    }

    if (!QTest::qWaitFor(
            [&]() {
                return player.playbackState() == DragonPlayer::PlaybackState::StoppedState;
            },
            stopTimeoutMs)) {
        qWarning() << "Player did not reach StoppedState within timeout";
        return false;
    }

    QTest::qWait(drainWaitMs);
    return true;
}

// Full PCM verification for a captured gapless stream: locates the end/start
// boundary markers, checks the inter-track gap, verifies track A/B boundary
// completeness and compares tail/head samples against the expected fixture data.
[[nodiscard]] inline bool
verifyGaplessPcm(const std::vector<float> &capturedPcm, const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int channels)
{
    using namespace FixtureGenerator;
    using namespace Qt::StringLiterals;

    const int capturedFrames = static_cast<int>(capturedPcm.size()) / channels;

    qDebug() << "PCM verification:"
             << "capturedFrames=" << capturedFrames << "expectedFramesA=" << fixtureA.totalFrames << "expectedFramesB=" << fixtureB.totalFrames
             << "totalExpected=" << (fixtureA.totalFrames + fixtureB.totalFrames);

    if (capturedFrames <= 0) {
        qWarning() << "Captured PCM should not be empty";
        return false;
    }

    auto endMarkerHit = findSignature(capturedPcm, channels, kEndSignature, true);
    if (!(endMarkerHit.found)) {
        qWarning().noquote() << qPrintable(u"Track A end marker not found in captured PCM (searched %1 frames)"_s.arg(capturedFrames));
        return false;
    }

    auto startMarkerHit = findSignature(capturedPcm, channels, kStartSignature, false);
    if (!(startMarkerHit.found)) {
        qWarning().noquote() << qPrintable(u"Track B start marker not found in captured PCM (searched %1 frames)"_s.arg(capturedFrames));
        return false;
    }

    if (!(endMarkerHit.frameIndex < startMarkerHit.frameIndex)) {
        qWarning().noquote() << qPrintable(
            u"End marker (frame %1) must precede start marker (frame %2)"_s.arg(endMarkerHit.frameIndex).arg(startMarkerHit.frameIndex));
        return false;
    }

    const qint64 gapFrames = startMarkerHit.frameIndex - (endMarkerHit.frameIndex + static_cast<int>(kEndSignature.size()));

    qDebug() << "Marker analysis:"
             << "endMarkerFrame=" << endMarkerHit.frameIndex << "endMarkerAmplitude=" << endMarkerHit.amplitude
             << "startMarkerFrame=" << startMarkerHit.frameIndex << "startMarkerAmplitude=" << startMarkerHit.amplitude << "gapFrames=" << gapFrames;

    const int maxGapFrames = fixtureA.sampleRate / 100;
    if (!(gapFrames <= maxGapFrames)) {
        qWarning().noquote() << qPrintable(u"Gap between tracks should be <= %1 frames (%2 ms), got %3 frames (%4 ms)"_s.arg(maxGapFrames)
                                               .arg(static_cast<qint64>(maxGapFrames) * 1000 / fixtureA.sampleRate)
                                               .arg(gapFrames)
                                               .arg(static_cast<qint64>(gapFrames) * 1000 / fixtureA.sampleRate));
        return false;
    }

    // Track A completeness: verify track A played to its very last sample
    int firstAudioFrame = -1;
    constexpr float silenceThreshold = 0.001f;
    for (int f = 0; f < capturedFrames; ++f) {
        if (std::abs(capturedPcm[f * channels]) > silenceThreshold) {
            firstAudioFrame = f;
            break;
        }
    }
    if (!(firstAudioFrame >= 0)) {
        qWarning().noquote() << "No audio content found in capture";
        return false;
    }
    qDebug() << "Leading silence:" << firstAudioFrame << "frames";

    if (!(std::abs(endMarkerHit.amplitude - 1.0f) < 0.01f)) {
        qWarning().noquote() << qPrintable(u"Track A end marker amplitude should be +1.0, got %1"_s.arg(endMarkerHit.amplitude));
        return false;
    }
    qDebug() << "Track A end marker verified at frame" << endMarkerHit.frameIndex << "(amplitude=" << endMarkerHit.amplitude << ")";

    // Track B completeness: verify track B starts at its very first sample
    if (!(std::abs(startMarkerHit.amplitude - (-1.0f)) < 0.01f)) {
        qWarning().noquote() << qPrintable(u"Track B start marker amplitude should be -1.0, got %1"_s.arg(startMarkerHit.amplitude));
        return false;
    }

    const qint64 trackBStartOffset = startMarkerHit.frameIndex;
    qDebug() << "Track B start marker verified at frame" << trackBStartOffset << "(amplitude=" << startMarkerHit.amplitude << ")";

    // Track B tail completeness: verify track B plays to completion
    int lastNonSilenceFrame = -1;
    for (int f = capturedFrames - 1; f >= static_cast<int>(trackBStartOffset); --f) {
        if (std::abs(capturedPcm[f * channels]) > silenceThreshold) {
            lastNonSilenceFrame = f;
            break;
        }
    }
    if (!(lastNonSilenceFrame > 0)) {
        qWarning().noquote() << "Track B has no audio content after start marker";
        return false;
    }

    const qint64 trackBFramesInCapture = lastNonSilenceFrame - trackBStartOffset + 1;
    qDebug() << "Track B completeness:"
             << "framesInCapture=" << trackBFramesInCapture << "expectedFrames=" << fixtureB.totalFrames;

    const int drainToleranceFrames = fixtureA.sampleRate / kFlacDrainToleranceDivisor;
    if (!(trackBFramesInCapture >= fixtureB.totalFrames - drainToleranceFrames)) {
        qWarning().noquote() << qPrintable(u"Track B truncated: captured %1 frames, expected %2 (lost %3 frames)"_s.arg(trackBFramesInCapture)
                                               .arg(fixtureB.totalFrames)
                                               .arg(fixtureB.totalFrames - trackBFramesInCapture));
        return false;
    }

    // Compare the last N frames of track A in captured PCM against expected samples.
    // Account for 16-bit FLAC quantization (tolerance ~1/32768 ~= 0.00003).
    const int tailStartCapture = static_cast<int>(endMarkerHit.frameIndex) - kVerifyEdgeFrames;
    const int tailStartExpected = fixtureA.totalFrames - static_cast<int>(kEndSignature.size()) - kVerifyEdgeFrames;
    if (tailStartCapture >= 0 && tailStartExpected >= 0) {
        if (!(verifySamples(capturedPcm,
                            tailStartCapture,
                            fixtureA.expectedSamples,
                            tailStartExpected,
                            kVerifyEdgeFrames,
                            channels,
                            kFlacQuantTolerance,
                            "Track A tail:"))) {
            qWarning().noquote() << "Track A tail samples mismatched";
            return false;
        }
    }

    const int headStartCapture = static_cast<int>(trackBStartOffset);
    if (headStartCapture + kVerifyEdgeFrames <= capturedFrames) {
        if (!(verifySamples(capturedPcm, headStartCapture, fixtureB.expectedSamples, 0, kVerifyEdgeFrames, channels, kFlacQuantTolerance, "Track B head:"))) {
            qWarning().noquote() << "Track B head samples mismatched";
            return false;
        }
    }

    return true;
}

} // namespace GaplessTestUtils
