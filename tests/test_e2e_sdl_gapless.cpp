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

#include "player/dragondiagnostics.h"
#include <DragonPlayer>

#include <cmath>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;
using namespace GaplessTestUtils;

static constexpr int kSampleRate = 44100;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 132300; // ~3 seconds at 44100Hz

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
    void testGaplessFormatChangeStateMachine();
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
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(runGaplessPlayback(player, helper, diagnostics, fixtureA, fixtureB, nullptr, 15000, 2000));

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");

    // on Windows it must be stopped to flush pcm to disk
    player.stop();

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");

    auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
    const int capturedFrames = static_cast<int>(capturedPcm.size()) / kDefaultChannels;
    const int expectedTotalFrames = fixtureA.totalFrames + fixtureB.totalFrames;
    QVERIFY2(capturedFrames >= expectedTotalFrames,
             qPrintable(u"Captured frame count %1 should be >= expected %2 (disk driver may pad with silence)"_s.arg(capturedFrames).arg(expectedTotalFrames)));

    QVERIFY(verifyGaplessPcm(capturedPcm, fixtureA, fixtureB, kDefaultChannels));

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
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(runGaplessPlayback(player, helper, diagnostics, fixtureA, fixtureB, nullptr, 15000, 2000));

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");
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

    // on Windows it must be stopped to flush pcm to disk
    player.stop();

    QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
    auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
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
        DragonDiagnostics diagnostics(&player);
        PlayerHelper helper(&player);
        QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
        QVERIFY(helper.playAndWait());
        VERIFY_AUDIO_ACTIVE(diagnostics);
        QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);
        QTest::qWait(2000); // let the disk driver drain its device buffer
        player.stop();

        QVERIFY2(QFileInfo::exists(m_pcmCapturePath), "SDL disk driver should have written PCM capture file");
        const auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
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
        DragonDiagnostics diagnostics(&player);
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
        const auto capturedPcm = readRawS16LeAsFloat(m_pcmCapturePath);
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
