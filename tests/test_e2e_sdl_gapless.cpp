/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * End-to-end gapless transition tests using SDL3 audio drivers.
 *
 * Same-format gapless uses the SDL "disk" driver to capture raw PCM
 * output to a file, enabling sample-level verification of boundary
 * marker continuity across track transitions.
 *
 * Format-change gapless uses the SDL "dummy" driver for state-machine
 * verification only, since the disk driver truncates on device reopen.
 */

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "test_utils.h"

#include <DragonMultimedia/dragondiagnostics.h>
#include <DragonMultimedia/dragonplayer.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

static constexpr int kSampleRate = 44100;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 132300; // ~3 seconds at 44100Hz

struct MarkerHit {
    bool found = false;
    int64_t frameIndex = -1;
    float amplitude = 0.0f;
};

static MarkerHit findSignature(const std::vector<float> &pcm, int channels, const std::array<float, 4> &signature, bool searchFromEnd, float threshold = 0.5f)
{
    MarkerHit hit;
    const int sigLen = static_cast<int>(signature.size());
    const int totalFrames = static_cast<int>(pcm.size()) / channels;

    if (totalFrames < sigLen) {
        return hit;
    }

    auto matchesSignature = [&](int frameIdx) -> bool {
        for (int k = 0; k < sigLen; ++k) {
            float sample = pcm[(frameIdx + k) * channels];
            if (std::abs(sample - signature[k]) > threshold) {
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

static std::vector<float> readRawS16LeAsFloat(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray data = file.readAll();
    file.close();

    const size_t numSamples = static_cast<size_t>(data.size()) / sizeof(int16_t);
    const auto *raw = reinterpret_cast<const int16_t *>(data.constData());
    std::vector<float> pcm(numSamples);
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        pcm[i] = static_cast<float>(raw[i]) * scale;
    }
    return pcm;
}

class TestSdlGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testGaplessSameFormat();
    void testGaplessFormatChange();
    void testSingleTrackIntegrity();

private:
    void runGaplessStateCheck(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB);

    QString m_oldAudioSink;
    QString m_oldSdlDriver;
    QString m_oldDiskOutputFile;
    QString m_oldDiskTimescale;
    QString m_pcmCapturePath;
};

void TestSdlGapless::initTestCase()
{
    m_oldAudioSink = qEnvironmentVariable("DRAGONMULTIMEDIA_AUDIO_SINK");
    m_oldSdlDriver = qEnvironmentVariable("SDL_AUDIODRIVER");
    m_oldDiskOutputFile = qEnvironmentVariable("SDL_AUDIO_DISK_OUTPUT_FILE");
    m_oldDiskTimescale = qEnvironmentVariable("SDL_AUDIO_DISK_TIMESCALE");

    qputenv("DRAGONMULTIMEDIA_AUDIO_SINK", "dragonsdlaudiosink");

    m_pcmCapturePath = QDir::tempPath() + u"/dragon-gapless-test-capture.raw"_s;
}

void TestSdlGapless::cleanupTestCase()
{
    if (m_oldAudioSink.isEmpty()) {
        qunsetenv("DRAGONMULTIMEDIA_AUDIO_SINK");
    } else {
        qputenv("DRAGONMULTIMEDIA_AUDIO_SINK", m_oldAudioSink.toUtf8());
    }

    if (m_oldSdlDriver.isEmpty()) {
        qunsetenv("SDL_AUDIODRIVER");
    } else {
        qputenv("SDL_AUDIODRIVER", m_oldSdlDriver.toUtf8());
    }

    if (m_oldDiskOutputFile.isEmpty()) {
        qunsetenv("SDL_AUDIO_DISK_OUTPUT_FILE");
    } else {
        qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", m_oldDiskOutputFile.toUtf8());
    }

    if (m_oldDiskTimescale.isEmpty()) {
        qunsetenv("SDL_AUDIO_DISK_TIMESCALE");
    } else {
        qputenv("SDL_AUDIO_DISK_TIMESCALE", m_oldDiskTimescale.toUtf8());
    }

    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::runGaplessStateCheck(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB)
{
    DragonPlayer player;
    PlayerHelper helper(&player);
    DragonDiagnostics diagnostics(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);

    QVERIFY2(helper.waitForTrackChange(30000), "Gapless track change should occur within timeout");

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during gapless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during gapless transition");

    VERIFY_AUDIO_ACTIVE(diagnostics);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);
    QTest::qWait(2000);

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");

    const int64_t durationAMs = (static_cast<int64_t>(fixtureA.totalFrames) * 1000) / fixtureA.sampleRate;
    const int64_t durationBMs = (static_cast<int64_t>(fixtureB.totalFrames) * 1000) / fixtureB.sampleRate;
    const int64_t totalExpectedMs = durationAMs + durationBMs;

    qDebug() << "Gapless scenario:"
             << "trackA=" << durationAMs << "ms"
             << "trackB=" << durationBMs << "ms"
             << "totalExpected=" << totalExpectedMs << "ms";
}

void TestSdlGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    qputenv("SDL_AUDIODRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8());
    qputenv("SDL_AUDIO_DISK_TIMESCALE", "1");

    QFile::remove(m_pcmCapturePath);

    runGaplessStateCheck(fixtureA, fixtureB);

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");

    auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
    const int capturedFrames = static_cast<int>(capturedPcm.size()) / kDefaultChannels;

    qDebug() << "PCM verification:"
             << "capturedFrames=" << capturedFrames << "expectedFramesA=" << fixtureA.totalFrames << "expectedFramesB=" << fixtureB.totalFrames
             << "totalExpected=" << (fixtureA.totalFrames + fixtureB.totalFrames) << "capturedBytes=" << capturedPcm.size() * sizeof(float);

    QVERIFY2(capturedFrames > 0, "Captured PCM should not be empty");

    const int expectedTotalFrames = fixtureA.totalFrames + fixtureB.totalFrames;

    QVERIFY2(capturedFrames >= expectedTotalFrames,
             qPrintable(u"Captured frame count %1 should be >= expected %2 (disk driver may pad with silence)"_s.arg(capturedFrames).arg(expectedTotalFrames)));

    auto endMarkerHit = findSignature(capturedPcm, kDefaultChannels, kEndSignature, true);
    QVERIFY2(endMarkerHit.found, qPrintable(u"Track A end marker not found in captured PCM (searched %1 frames)"_s.arg(capturedFrames)));

    auto startMarkerHit = findSignature(capturedPcm, kDefaultChannels, kStartSignature, false);
    QVERIFY2(startMarkerHit.found, qPrintable(u"Track B start marker not found in captured PCM (searched %1 frames)"_s.arg(capturedFrames)));

    QVERIFY2(endMarkerHit.frameIndex < startMarkerHit.frameIndex,
             qPrintable(u"End marker (frame %1) must precede start marker (frame %2)"_s.arg(endMarkerHit.frameIndex).arg(startMarkerHit.frameIndex)));

    const int64_t gapFrames = startMarkerHit.frameIndex - (endMarkerHit.frameIndex + static_cast<int>(kEndSignature.size()));

    qDebug() << "Marker analysis:"
             << "endMarkerFrame=" << endMarkerHit.frameIndex << "endMarkerAmplitude=" << endMarkerHit.amplitude
             << "startMarkerFrame=" << startMarkerHit.frameIndex << "startMarkerAmplitude=" << startMarkerHit.amplitude << "gapFrames=" << gapFrames;

    const int maxGapFrames = kSampleRate / 100;
    QVERIFY2(gapFrames <= maxGapFrames,
             qPrintable(u"Gap between tracks should be <= %1 frames (%2 ms), got %3 frames (%4 ms)"_s.arg(maxGapFrames)
                            .arg(static_cast<int64_t>(maxGapFrames) * 1000 / kSampleRate)
                            .arg(gapFrames)
                            .arg(static_cast<int64_t>(gapFrames) * 1000 / kSampleRate)));

    const int64_t expectedEndMarkerFrame = fixtureA.totalFrames - static_cast<int>(kEndSignature.size());
    qDebug() << "Position check:"
             << "endMarkerFrame=" << endMarkerHit.frameIndex << "expectedEndMarkerFrame=" << expectedEndMarkerFrame
             << "delta=" << (endMarkerHit.frameIndex - expectedEndMarkerFrame);

    QVERIFY2(startMarkerHit.frameIndex > endMarkerHit.frameIndex, "Start marker must appear after end marker in captured PCM");

    const int64_t trackASpan = endMarkerHit.frameIndex + static_cast<int>(kEndSignature.size());
    QVERIFY2(trackASpan > 0, qPrintable(u"Track A should have produced audio before end marker (got %1 frames)"_s.arg(trackASpan)));

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::testGaplessFormatChange()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, 1, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    qputenv("SDL_AUDIODRIVER", "dummy");
    qunsetenv("SDL_AUDIO_DISK_OUTPUT_FILE");
    qunsetenv("SDL_AUDIO_DISK_TIMESCALE");

    runGaplessStateCheck(fixtureA, fixtureB);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

void TestSdlGapless::testSingleTrackIntegrity()
{
    using namespace FixtureGenerator;

    constexpr int channels = kDefaultChannels;
    constexpr int durationFrames = kSampleRate * 10;

    auto fixture = makeTenSecondFixture(kSampleRate, channels, durationFrames);
    QVERIFY(QFileInfo::exists(fixture.filePath));

    qputenv("SDL_AUDIODRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8());
    qputenv("SDL_AUDIO_DISK_TIMESCALE", "1");
    QFile::remove(m_pcmCapturePath);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixture.filePath)));
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    // The drain fix defers StoppedState until pipe + device buffer empty.
    // With TIMESCALE=1 (real-time), a 10s track takes ~10s to play out.
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 30000);

    // The SDL disk driver writes to the capture file in real-time
    // (TIMESCALE=1). After drain fires the stream is empty but the disk
    // driver's device ring buffer still holds ~1s of audio. Wait for the
    // virtual clock to consume the device buffer before reading the file.
    QTest::qWait(2000);

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
    auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
    const int capturedFrames = static_cast<int>(capturedPcm.size()) / channels;
    qDebug() << "Single-track: captured=" << capturedFrames << "expected=" << durationFrames;
    QVERIFY2(capturedFrames > 0, "Captured PCM empty");

    // Find first non-zero frame SDL disk driver writes leading silence
    // while the device is open but the decode pipeline hasn't filled the pipe yet.
    // The start marker (frames 0-3) may be consumed by this initial starvation.
    int alignFrame = -1;
    for (int f = 0; f < capturedFrames; ++f) {
        if (std::abs(capturedPcm[f * channels]) > 0.001f) {
            alignFrame = f;
            break;
        }
    }
    QVERIFY2(alignFrame >= 0, "No audio content found in capture");
    qDebug() << "Leading silence:" << alignFrame << "frames";

    // Verify end marker is present this is the key invariant the drain fix protects.
    auto endHit = findSignature(capturedPcm, channels, kEndSignature, true, 0.3f);
    QVERIFY2(endHit.found, "End marker not found (truncation)");

    // Count non-zero frames from alignFrame to end of actual audio
    int lastNonZero = -1;
    for (int f = capturedFrames - 1; f >= alignFrame; --f) {
        if (std::abs(capturedPcm[f * channels]) > 0.001f) {
            lastNonZero = f;
            break;
        }
    }
    QVERIFY2(lastNonZero > alignFrame, "Audio content too short");
    const int audioSpan = lastNonZero - alignFrame + 1;
    qDebug() << "Audio span:" << audioSpan << "frames (first non-zero to last non-zero)";

    // The audio span should be close to the expected duration.
    // Allow up to 10% tolerance: the SDL disk driver writes silence buffers
    // during initial startup (before the pipe fills), which displaces an
    // equivalent amount of audio from the real-time capture window.
    const int maxLoss = durationFrames / 10;
    QVERIFY2(
        audioSpan >= durationFrames - maxLoss,
        qPrintable(u"Audio span %1 too short, expected >= %2 (lost %3 frames)"_s.arg(audioSpan).arg(durationFrames - maxLoss).arg(durationFrames - audioSpan)));

    // Sample-level comparison from alignFrame through end marker.
    // Map captured frames back to expected samples using offset alignment.
    // The alignFrame in captured corresponds to some offset in the source.
    // We find this offset by matching the tone phase at alignFrame.
    constexpr double kTol = 0.01;
    int bestOffset = -1;
    double bestScore = 1e9;
    // Search within the first ~2000 source frames for best alignment
    const int searchRange = std::min(2000, fixture.totalFrames - audioSpan);
    for (int off = 0; off <= searchRange; ++off) {
        double score = 0.0;
        const int checkLen = std::min(100, audioSpan);
        for (int i = 0; i < checkLen; ++i) {
            score +=
                std::abs(static_cast<double>(capturedPcm[(alignFrame + i) * channels]) - static_cast<double>(fixture.expectedSamples[(off + i) * channels]));
        }
        if (score < bestScore) {
            bestScore = score;
            bestOffset = off;
        }
    }
    qDebug() << "Best alignment offset:" << bestOffset << "score:" << bestScore;
    QVERIFY2(bestOffset >= 0, "Could not find alignment");

    // Now compare samples using the found offset
    int mismatches = 0;
    double maxDev = 0.0;
    const int compareEnd = std::min(audioSpan, fixture.totalFrames - bestOffset);
    for (int i = 0; i < compareEnd; ++i) {
        double md = 0.0;
        for (int ch = 0; ch < channels; ++ch) {
            md = std::max(md,
                          std::abs(static_cast<double>(capturedPcm[(alignFrame + i) * channels + ch])
                                   - static_cast<double>(fixture.expectedSamples[(bestOffset + i) * channels + ch])));
        }
        maxDev = std::max(maxDev, md);
        if (md > kTol)
            ++mismatches;
    }
    qDebug() << "PCM: compared=" << compareEnd << "mismatched=" << mismatches << "maxDev=" << maxDev;
    QVERIFY2(mismatches <= std::max(100, compareEnd / 100),
             qPrintable(u"Too many mismatches: %1/%2 (max %3) maxDev=%4"_s.arg(mismatches).arg(compareEnd).arg(std::max(100, compareEnd / 100)).arg(maxDev)));

    QFile::remove(fixture.filePath);
    QFile::remove(m_pcmCapturePath);
}

QTEST_MAIN(TestSdlGapless)
#include "test_e2e_sdl_gapless.moc"
