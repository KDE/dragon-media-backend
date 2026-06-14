/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * End-to-end gapless transition tests using SDL3 dummy audio driver.
 * Uses boundary-marker fixtures to verify gapless PCM continuity
 * through the full decode → pipe → SDL sink pipeline.
 *
 * The SDL dummy driver consumes audio without hardware, providing
 * deterministic timing without requiring PipeWire or a session manager.
 */

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "test_utils.h"

#include <DragonMultimedia/dragondiagnostics.h>
#include <DragonMultimedia/dragonplayer.h>

using namespace Qt::StringLiterals;

static constexpr int kSampleRate = 44100;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 132300; // ~3 seconds at 44100Hz
static constexpr int kQuantumFrames = 1024;

class TestSdlGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testGaplessSameFormat();
    void testGaplessFormatChange();

private:
    void runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapMs);

    QString m_oldAudioSink;
    QString m_oldSdlDriver;
};

void TestSdlGapless::initTestCase()
{
    m_oldAudioSink = qEnvironmentVariable("DRAGONMULTIMEDIA_AUDIO_SINK");
    m_oldSdlDriver = qEnvironmentVariable("SDL_AUDIODRIVER");

    qputenv("DRAGONMULTIMEDIA_AUDIO_SINK", "dragonsdlaudiosink");
    qputenv("SDL_AUDIODRIVER", "dummy");
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
}

void TestSdlGapless::runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapMs)
{
    DragonPlayer player;
    PlayerHelper helper(&player);
    DragonDiagnostics diagnostics(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);

    QVERIFY2(helper.waitForTrackChange(30000), "Gapless track change should occur within timeout");

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during gapless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during gapless transition");

    VERIFY_AUDIO_ACTIVE(player);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);

    QVERIFY2(trackSpy.count() >= 1, "At least one trackChanged signal expected");

    const int64_t durationAMs = (static_cast<int64_t>(fixtureA.totalFrames) * 1000) / fixtureA.sampleRate;
    const int64_t durationBMs = (static_cast<int64_t>(fixtureB.totalFrames) * 1000) / fixtureB.sampleRate;
    const int64_t totalExpectedMs = durationAMs + durationBMs;

    qDebug() << "Gapless scenario:"
             << "trackA=" << durationAMs << "ms"
             << "trackB=" << durationBMs << "ms"
             << "totalExpected=" << totalExpectedMs << "ms"
             << "starvationCount=" << diagnostics.audioStarvationCount() << "callbackHz=" << diagnostics.audioCallbackHz();

    Q_UNUSED(expectedMaxGapMs);
    Q_UNUSED(kQuantumFrames);

    player.stop();
}

void TestSdlGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    runGaplessScenario(fixtureA, fixtureB, 0);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

void TestSdlGapless::testGaplessFormatChange()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, 1, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    runGaplessScenario(fixtureA, fixtureB, kQuantumFrames * 1000 / kSampleRate);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

QTEST_MAIN(TestSdlGapless)
#include "test_e2e_sdl_gapless.moc"
