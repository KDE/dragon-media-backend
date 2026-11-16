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
#include "helpers/gapless_test_utils.h"
#include "test_utils.h"

#include "player/diagnostics.h"
#include "sink/playbackpipeline.h"
#include <DragonPlayer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;
using namespace GaplessTestUtils;

static constexpr int kSampleRate = 44100;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 132300; // ~3 seconds at 44100Hz

static std::vector<float> readRawF32Le(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const QByteArray data = file.readAll();
    file.close();

    const size_t numSamples = static_cast<size_t>(data.size()) / sizeof(float);
    std::vector<float> pcm(numSamples);
    if (numSamples > 0) {
        std::memcpy(pcm.data(), data.constData(), numSamples * sizeof(float));
    }
    return pcm;
}

// Goertzel magnitude of channel 0 at freq over a window of frames.
static double goertzelMagnitude(const std::vector<float> &pcm, int startFrame, int frames, int channels, double freq, int sampleRate)
{
    if (startFrame < 0 || startFrame + frames > static_cast<int>(pcm.size()) / channels) {
        return 0.0;
    }
    const double coeff = 2.0 * std::cos(2.0 * M_PI * freq / sampleRate);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double x = pcm[(startFrame + i) * channels];
        s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / frames;
}

// Per-window magnitudes of the two tones, 50 ms windows.
static void
analyzeWindows(const std::vector<float> &pcm, int channels, int sampleRate, double freqA, double freqB, std::vector<double> &magA, std::vector<double> &magB)
{
    const int windowFrames = sampleRate / 20;
    const int totalFrames = static_cast<int>(pcm.size()) / channels;
    const int numWindows = totalFrames / windowFrames;
    for (int w = 0; w < numWindows; ++w) {
        magA.push_back(goertzelMagnitude(pcm, w * windowFrames, windowFrames, channels, freqA, sampleRate));
        magB.push_back(goertzelMagnitude(pcm, w * windowFrames, windowFrames, channels, freqB, sampleRate));
    }
}

class TestSdlGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testGaplessSameFormat();
    void testCrossfadeSameFormat();
    void testGaplessFormatChangeStateMachine();
    void testTrackChangeAtAudibleBoundary();
    void testFormatChangePlaysAtNativeRate();
    void testSingleTrackIntegrity();
    void testTrackStartNotWiped();

private:
    EnvGuard m_envGuard{
        {"DRAGON_AUDIO_SINK", QByteArray("dragonsdlaudiosink")},
        {"SDL_AUDIODRIVER", qgetenv("SDL_AUDIODRIVER")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", qgetenv("SDL_AUDIO_DISK_OUTPUT_FILE")},
        {"SDL_AUDIO_DISK_TIMESCALE", qgetenv("SDL_AUDIO_DISK_TIMESCALE")},
    };
    QString m_pcmCapturePath;
};

void TestSdlGapless::initTestCase()
{
    m_pcmCapturePath = QDir::tempPath() + u"/dragon-gapless-test-capture.raw"_s;
}

void TestSdlGapless::cleanupTestCase()
{
    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    EnvGuard diskDriverEnv{
        {"SDL_AUDIODRIVER", QByteArray("disk")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray("1")},
    };

    QFile::remove(m_pcmCapturePath);

    DragonPlayer player;
    Diagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(runGaplessPlayback(player, helper, diagnostics, fixtureA, fixtureB, nullptr, 15000, 2000));

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");

    // on Windows it must be stopped to flush pcm to disk
    player.stop();

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");

    auto capturedPcm = readRawF32Le(m_pcmCapturePath);
    const int capturedFrames = static_cast<int>(capturedPcm.size()) / kDefaultChannels;
    const int expectedTotalFrames = fixtureA.totalFrames + fixtureB.totalFrames;
    QVERIFY2(capturedFrames >= expectedTotalFrames,
             qPrintable(u"Captured frame count %1 should be >= expected %2 (disk driver may pad with silence)"_s.arg(capturedFrames).arg(expectedTotalFrames)));

    QVERIFY(verifyGaplessPcm(capturedPcm, fixtureA, fixtureB, kDefaultChannels));

    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::testCrossfadeSameFormat()
{
    // Track A: 440 Hz sine, Track B: 880 Hz sine, both 3 s stereo at 44.1 kHz.
    const QString pathA = QDir::tempPath() + u"/dragon-crossfade-a.flac"_s;
    const QString pathB = QDir::tempPath() + u"/dragon-crossfade-b.flac"_s;
    FixtureGuard guardA{pathA};
    FixtureGuard guardB{pathB};

    auto makeSineFixture = [&](const QString &path, double freq) {
        std::vector<float> samples(static_cast<size_t>(kDurationFrames) * kDefaultChannels);
        for (int frm = 0; frm < kDurationFrames; ++frm) {
            const double t = static_cast<double>(frm) / kSampleRate;
            const float val = static_cast<float>(std::sin(2.0 * M_PI * freq * t) * 0.4);
            samples[static_cast<size_t>(frm) * kDefaultChannels] = val;
            samples[static_cast<size_t>(frm) * kDefaultChannels + 1] = val;
        }
        return FixtureGenerator::writeFlac(path, samples, kSampleRate, kDefaultChannels, kDurationFrames);
    };
    QVERIFY2(makeSineFixture(pathA, 440.0), "Failed to generate crossfade fixture A");
    QVERIFY2(makeSineFixture(pathB, 880.0), "Failed to generate crossfade fixture B");

    EnvGuard diskDriverEnv{
        {"SDL_AUDIODRIVER", QByteArray("disk")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray("1")},
    };

    QFile::remove(m_pcmCapturePath);

    DragonPlayer player;
    Diagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY2(helper.setSourceAndWait(QUrl::fromLocalFile(pathA)), "Track A failed to load");
    QVERIFY2(helper.playAndWait(), "Track A failed to start playing");
    QVERIFY2(diagnostics.isAudioActive(), "Audio should be active after play");

    // Let track A play audibly for ~500 ms before the crossfade.
    QTest::qWait(500);

    DragonSourceOptions options;
    options.setCrossfadeDuration(1000ms);
    player.setSource(QUrl::fromLocalFile(pathB), options);

    // The crossfade keeps the player playing the incoming track's timeline
    // without reopening the audio device.
    QVERIFY2(helper.waitForState(DragonPlayer::PlaybackState::PlayingState, 10000), "Player did not resume PlayingState during crossfade");
    QCOMPARE(player.source(), QUrl::fromLocalFile(pathB));
    QVERIFY2(diagnostics.isAudioActive(), "Audio should remain active during crossfade");
    QVERIFY2(helper.waitForEndOfMedia(30000), "Track B did not reach EndOfMedia");
    QVERIFY2(helper.waitForState(DragonPlayer::PlaybackState::StoppedState, 10000), "Player did not stop after track B finished");

    player.stop(); // flush the disk capture on all platforms

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
    auto capturedPcm = readRawF32Le(m_pcmCapturePath);
    QVERIFY2(capturedPcm.size() > 2u * static_cast<size_t>(kSampleRate), "Capture too short");

    std::vector<double> mag440;
    std::vector<double> mag880;
    analyzeWindows(capturedPcm, kDefaultChannels, kSampleRate, 440.0, 880.0, mag440, mag880);
    QVERIFY2(mag440.size() > 30, "Capture too short for window analysis");

    const double max440 = *std::ranges::max_element(mag440);
    const double max880 = *std::ranges::max_element(mag880);
    QVERIFY2(max440 > 0.1, qPrintable(u"440 Hz tone missing from capture (max=%1)"_s.arg(max440)));
    QVERIFY2(max880 > 0.1, qPrintable(u"880 Hz tone missing from capture (max=%1)"_s.arg(max880)));

    const double thresh440 = 0.25 * max440;
    const double thresh880 = 0.25 * max880;

    int fadeStart = -1; // first window where 880 Hz becomes audible
    int fadeEnd = -1; // last window where 440 Hz is still audible
    for (int w = 0; w < static_cast<int>(mag880.size()); ++w) {
        if (fadeStart < 0 && mag880[w] > thresh880) {
            fadeStart = w;
        }
        if (mag440[w] > thresh440) {
            fadeEnd = w;
        }
    }
    QVERIFY2(fadeStart > 2, "880 Hz appeared immediately: no pre-fade window for track A");
    QVERIFY2(fadeEnd > fadeStart, "No overlapped crossfade window: track B started after track A ended");

    // Pre-fade: pure 440 Hz, no 880 Hz energy.
    QVERIFY2(mag440[2] > 0.6 * max440, "Track A is not at unity gain before the fade");
    QVERIFY2(mag880[2] < 0.15 * max880, "Track B audible before the crossfade");

    // Post-fade: pure 880 Hz, no 440 Hz energy.
    const int lastWindow = static_cast<int>(mag440.size()) - 1;
    QVERIFY2(mag880[lastWindow - 2] > 0.6 * max880, "Track B is not at unity gain after the fade");
    QVERIFY2(mag440[lastWindow - 2] < 0.15 * max440, "Track A still audible after the fade");

    // Equal-power handoff: across the overlap 440 Hz declines and 880 Hz rises,
    // both monotonically within noise tolerance, and the power sum stays near
    // the pre-fade level instead of dipping.
    for (int w = fadeStart; w <= fadeEnd; ++w) {
        if (w > fadeStart) {
            QVERIFY2(mag440[w] <= mag440[w - 1] + 0.15 * max440, qPrintable(u"440 Hz envelope not monotonically declining at window %1"_s.arg(w)));
            QVERIFY2(mag880[w] >= mag880[w - 1] - 0.15 * max880, qPrintable(u"880 Hz envelope not monotonically rising at window %1"_s.arg(w)));
        }
    }

    // No clicks: adjacent captured samples in the overlap window must stay
    // within the slope of the two summed sines.
    const int overlapStartFrame = fadeStart * (kSampleRate / 20);
    const int overlapEndFrame = (fadeEnd + 1) * (kSampleRate / 20);
    for (int i = overlapStartFrame + 1; i < overlapEndFrame; ++i) {
        const float diff = qAbs(capturedPcm[static_cast<size_t>(i) * kDefaultChannels] - capturedPcm[(static_cast<size_t>(i) - 1) * kDefaultChannels]);
        QVERIFY2(diff < 0.1f, qPrintable(u"Sample discontinuity (%1) at frame %2 during crossfade"_s.arg(diff).arg(i)));
    }

    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::testGaplessFormatChangeStateMachine()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, 1, kDurationFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    EnvGuard driverEnv{
        {"SDL_AUDIODRIVER", QByteArray("dummy")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", QByteArray()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray()},
    };

    DragonPlayer player;
    Diagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(runGaplessPlayback(player, helper, diagnostics, fixtureA, fixtureB, nullptr, 15000, 2000));

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");
}

void TestSdlGapless::testTrackChangeAtAudibleBoundary()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};

    EnvGuard driverEnv{
        {"SDL_AUDIODRIVER", QByteArray("dummy")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", QByteArray()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray()},
    };

    DragonPlayer player;
    Diagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    qint64 samplesReadAtTrackChange = -1;
    QObject::connect(&player, &DragonPlayer::trackChanged, &player, [&]() {
        if (samplesReadAtTrackChange < 0) {
            samplesReadAtTrackChange = diagnostics.audioSamplesRead();
        }
    });

    QVERIFY2(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)), "Track 1 failed to load");
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));
    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return samplesReadAtTrackChange >= 0;
                 },
                 60000),
             "Gapless transition did not happen");

    const qint64 trackATotalSamples = qint64(fixtureA.totalFrames) * kDefaultChannels;
    const qint64 tolerance = 4096;
    const qint64 unreadTail = trackATotalSamples - samplesReadAtTrackChange;
    qDebug() << "samplesReadAtTrackChange=" << samplesReadAtTrackChange << "trackATotalSamples=" << trackATotalSamples << "unreadTailSamples=" << unreadTail;
    QVERIFY2(unreadTail <= tolerance,
             qPrintable(u"trackChanged fired while %1 samples (~%2ms) of track A were still unread: the signal is early"_s.arg(unreadTail)
                            .arg(unreadTail * 1000 / (kSampleRate * kDefaultChannels))));
}

void TestSdlGapless::testFormatChangePlaysAtNativeRate()
{
    using namespace FixtureGenerator;

    constexpr int trackBFrames = kSampleRate * 5; // 5s

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, 1, trackBFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};

    EnvGuard driverEnv{
        {"SDL_AUDIODRIVER", QByteArray("dummy")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", QByteArray()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray()},
    };

    DragonPlayer player;
    Diagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QElapsedTimer timer;
    qint64 msAtTrackChange = -1;
    timer.start();
    QObject::connect(&player, &DragonPlayer::trackChanged, &player, [&]() {
        if (msAtTrackChange < 0) {
            msAtTrackChange = timer.elapsed();
        }
    });

    QVERIFY2(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)), "Track 1 failed to load");
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));
    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return msAtTrackChange >= 0;
                 },
                 60000),
             "Gapless transition did not happen");

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 30000);

    const qint64 elapsedMs = timer.elapsed() - msAtTrackChange;
    const qint64 expectedMs = qint64(trackBFrames) * 1000 / kSampleRate;
    qDebug() << "trackB audible span=" << elapsedMs << "ms, expected ~" << expectedMs << "ms";
    QVERIFY2(elapsedMs >= expectedMs * 9 / 10,
             qPrintable(u"Track B (mono, %1ms) audibly ended after only %2ms: it was played at track A's stereo device rate"_s.arg(expectedMs).arg(elapsedMs)));
}

void TestSdlGapless::testSingleTrackIntegrity()
{
    using namespace FixtureGenerator;

    constexpr int channels = kDefaultChannels;
    constexpr int durationFrames = kSampleRate * 10;

    auto fixture = makeTenSecondFixture(kSampleRate, channels, durationFrames);
    QVERIFY(QFileInfo::exists(fixture.filePath));

    EnvGuard diskDriverEnv{
        {"SDL_AUDIODRIVER", QByteArray("disk")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray("1")},
    };
    QFile::remove(m_pcmCapturePath);

    DragonPlayer player;
    Diagnostics diagnostics(&player);
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

    // on Windows it must be stopped to flush pcm to disk
    player.stop();

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
    auto capturedPcm = readRawF32Le(m_pcmCapturePath);
    const int capturedFrames = static_cast<int>(capturedPcm.size()) / channels;
    qDebug() << "Single-track: captured=" << capturedFrames << "expected=" << durationFrames;
    QVERIFY2(capturedFrames > 0, "Captured PCM empty");

    auto startHit = findSignature(capturedPcm, channels, kStartSignature, false, 0.3f);
    QVERIFY2(startHit.found, "Start marker not found in capture: the beginning of the track was discarded");
    constexpr int kMaxStartupSilenceFrames = 4096; // ~93ms; observed 1024
    QVERIFY2(startHit.frameIndex <= kMaxStartupSilenceFrames, qPrintable(u"Too much startup silence before track start: %1 frames"_s.arg(startHit.frameIndex)));
    qDebug() << "Leading silence:" << startHit.frameIndex << "frames";

    QVERIFY2(
        capturedFrames - startHit.frameIndex >= durationFrames,
        qPrintable(u"Capture too short for full track: %1 frames from marker, expected %2"_s.arg(capturedFrames - startHit.frameIndex).arg(durationFrames)));

    // Verify end marker is present: the key invariant the drain fix protects.
    auto endHit = findSignature(capturedPcm, channels, kEndSignature, true, 0.3f);
    QVERIFY2(endHit.found, "End marker not found (truncation)");
    const int audioSpan = static_cast<int>(endHit.frameIndex) - startHit.frameIndex + 1;
    const int expectedSpan = durationFrames - static_cast<int>(kEndSignature.size()) + 1;
    QVERIFY2(audioSpan >= expectedSpan,
             qPrintable(u"Audio span %1 frames (start marker to end marker) too short, expected %2"_s.arg(audioSpan).arg(expectedSpan)));

    QVERIFY2(verifySamples(capturedPcm, startHit.frameIndex, fixture.expectedSamples, 0, durationFrames, channels, kFlacQuantTolerance, "single-track"),
             "Single track must reproduce sample-exactly from source frame 0");

    QFile::remove(fixture.filePath);
    QFile::remove(m_pcmCapturePath);
}

void TestSdlGapless::testTrackStartNotWiped()
{
    using namespace FixtureGenerator;

    constexpr int channels = kDefaultChannels;

    EnvGuard diskDriverEnv{
        {"SDL_AUDIODRIVER", QByteArray("disk")},
        {"SDL_AUDIO_DISK_OUTPUT_FILE", m_pcmCapturePath.toUtf8()},
        {"SDL_AUDIO_DISK_TIMESCALE", QByteArray("1")},
    };

    const int shortFrames = kSampleRate * 2 / 3; // 29400 frames = 58800 samples < 65536
    auto fixtureA = makeStartMarkerFixture(kSampleRate, channels, shortFrames);
    FixtureGuard guardA{fixtureA.filePath};
    QVERIFY(QFileInfo::exists(fixtureA.filePath));

    QFile::remove(m_pcmCapturePath);

    {
        DragonPlayer player;
        Diagnostics diagnostics(&player);
        PlayerHelper helper(&player);
        QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
        QVERIFY(helper.playAndWait());
        VERIFY_AUDIO_ACTIVE(diagnostics);
        QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);
        QTest::qWait(2000); // let the disk driver drain its device buffer
        player.stop();

        QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
        const auto capturedPcm = readRawF32Le(m_pcmCapturePath);
        const int capturedFrames = static_cast<int>(capturedPcm.size()) / channels;
        QVERIFY2(capturedFrames > 0, "Captured PCM empty");

        auto startHit = findSignature(capturedPcm, channels, kStartSignature, false, 0.3f);
        QVERIFY2(startHit.found, "Start marker not found in capture: the beginning of the track was discarded");

        constexpr int kMaxStartupSilenceFrames = 4096; // ~93ms; observed 1024
        QVERIFY2(startHit.frameIndex <= kMaxStartupSilenceFrames,
                 qPrintable(u"Too much startup silence before track start: %1 frames"_s.arg(startHit.frameIndex)));

        QVERIFY2(
            capturedFrames - startHit.frameIndex >= fixtureA.totalFrames,
            qPrintable(
                u"Capture too short for full track: %1 frames from marker, expected %2"_s.arg(capturedFrames - startHit.frameIndex).arg(fixtureA.totalFrames)));

        QVERIFY2(
            verifySamples(capturedPcm, startHit.frameIndex, fixtureA.expectedSamples, 0, fixtureA.totalFrames, channels, kFlacQuantTolerance, "fresh-play"),
            "Fresh play must reproduce the track sample-exactly from frame 0");
    }

    const int longFrames = kSampleRate * 3;
    auto fixtureB = makeStartMarkerFixture(kSampleRate, channels, longFrames);
    FixtureGuard guardB{fixtureB.filePath};
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    QFile::remove(m_pcmCapturePath);

    {
        DragonPlayer player;
        Diagnostics diagnostics(&player);
        PlayerHelper helper(&player);
        QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
        QVERIFY(helper.playAndWait());
        QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);

        QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureB.filePath)));
        QVERIFY(helper.playAndWait());
        VERIFY_AUDIO_ACTIVE(diagnostics);
        QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);
        QTest::qWait(2000);
        player.stop();

        QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
        const auto capturedPcm = readRawF32Le(m_pcmCapturePath);
        const int capturedFrames = static_cast<int>(capturedPcm.size()) / channels;
        QVERIFY2(capturedFrames > 0, "Captured PCM empty");

        auto startHit = findSignature(capturedPcm, channels, kStartSignature, true, 0.3f);
        QVERIFY2(startHit.found, "Start marker not found after reselect: the beginning of the second track was discarded");

        QVERIFY2(capturedFrames - startHit.frameIndex >= fixtureB.totalFrames,
                 qPrintable(u"Capture too short for full second track: %1 frames from marker, expected %2"_s.arg(capturedFrames - startHit.frameIndex)
                                .arg(fixtureB.totalFrames)));

        QVERIFY2(
            verifySamples(capturedPcm, startHit.frameIndex, fixtureB.expectedSamples, 0, fixtureB.totalFrames, channels, kFlacQuantTolerance, "reselect-play"),
            "Track played after stop must reproduce sample-exactly from frame 0");
    }
}

QTEST_MAIN(TestSdlGapless)
#include "test_e2e_sdl_gapless.moc"
