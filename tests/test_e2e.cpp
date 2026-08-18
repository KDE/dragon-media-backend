/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * These tests exercise the full DragonPlayer pipeline (decoder -> sink -> FFT)
 * and are executed for each audio backend.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "player/dragondiagnostics.h"
#include <DragonMultimedia/dragonplayer.h>
#include <DragonMultimedia/dragonspectrumanalyzer.h>

using namespace Qt::StringLiterals;

class TestE2E : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPlayerWithMp3File();
    void testPlayerWithOggFile();

    void testPlayerStopActuallyStopsAudio();
    void testPlayerPauseResumeSequence();

    void testSeamlessPlaybackTransition();

    void testSeamlessPlaybackWithFormatChange();
    void testFftFramesDuringGaplessTransition();

    void testTrackChangeWhilePaused();
    void testGaplessGenerationCheck();
    void testNonGaplessEofWithFftOn();

    void testSameFormatSeamlessTransition();

    void testGaplessTransitionCoroutine();
    void testGaplessFormatMismatch();
    void testGaplessPreWarmError();
    void testGaplessStarvation();
    void testDiagnosticsBasicFunctionality();
};

void TestE2E::testPlayerWithMp3File()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(player.source() == QUrl::fromLocalFile(path));

    QVERIFY2(durationSpy.count() >= 1, "durationChanged must be emitted after loading media");
    QVERIFY2(player.duration() > 0, "Duration should be positive after loading media");
    QVERIFY2(player.seekable(), "Player should be seekable after loading media");

    QVERIFY2(helper.playAndWait(), "Playback should start successfully");
    VERIFY_AUDIO_ACTIVE(diagnostics);

    player.stop();
}

void TestE2E::testPlayerWithOggFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    QVERIFY(player.seekable());
    QVERIFY2(player.duration() > 0, "Duration should be positive after loading OGG media");

    QVERIFY2(helper.playAndWait(), "Playback should start successfully for OGG");
    VERIFY_AUDIO_ACTIVE(diagnostics);

    player.stop();
}

void TestE2E::testPlayerStopActuallyStopsAudio()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    VERIFY_AUDIO_ACTIVE(diagnostics);
    QTRY_VERIFY(diagnostics.audioBufferFrames() >= 0);
    QTRY_VERIFY(diagnostics.audioBufferUs() >= 0);
    QVERIFY(diagnostics.hasActiveDecoder());
    QVERIFY(diagnostics.decodeLoopActive() || diagnostics.decodeQueueSize() > 0);

    player.stop();
    VERIFY_STOPPED_STATE(player);
    VERIFY_AUDIO_INACTIVE(diagnostics);

    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.audioBufferUs() == -1, 1000);
    QVERIFY2(diagnostics.audioBufferFrames() == -1, "Audio buffer frames should return -1 after stop");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder after stop");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active after stop");
}

void TestE2E::testPlayerPauseResumeSequence()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    VERIFY_AUDIO_ACTIVE(diagnostics);
    VERIFY_PLAYING_STATE(player);

    QVERIFY(helper.pauseAndWait());
    VERIFY_PAUSED_STATE(player);
    VERIFY_AUDIO_ACTIVE(diagnostics);

    QVERIFY(helper.playAndWait());
    VERIFY_PLAYING_STATE(player);
    VERIFY_AUDIO_ACTIVE(diagnostics);

    player.stop();
    VERIFY_STOPPED_STATE(player);
    VERIFY_AUDIO_INACTIVE(diagnostics);
}

void TestE2E::testSeamlessPlaybackTransition()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.m4a"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-3s-2c-44100hz.m4a"_s);

    QVERIFY(player.nextSource() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    const int underrunsBefore = diagnostics.audioUnderrunCount();
    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during seamless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during seamless transition");

    const int underrunsAfter = diagnostics.audioUnderrunCount();
    QVERIFY2(underrunsAfter - underrunsBefore == 0,
             qPrintable(u"Audio should not underrun during seamless transition: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    VERIFY_AUDIO_ACTIVE(diagnostics);
    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));
    QVERIFY(!player.nextSource().isValid());
    QVERIFY2(sourceSpy.count() >= 2, qPrintable(u"Expected at least 2 source changes, got %1"_s.arg(sourceSpy.count())));

    qDebug() << "Seamless playback test passed:"
             << "trackChanged=" << trackSpy.count() << "stateChanges=" << stateSpy.count() << "sourceChanges=" << sourceSpy.count();

    player.stop();
}

void TestE2E::testSeamlessPlaybackWithFormatChange()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-1c-44100hz.flac"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-3s-1c-44100hz.flac"_s);

    QVERIFY(player.nextSource().isValid());
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QTest::qWait(1000);
    const int underrunsAfter = diagnostics.audioUnderrunCount();

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during seamless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during seamless transition");
    QVERIFY2(underrunsAfter - underrunsBefore == 0,
             qPrintable(u"Audio should not underrun during format-change gapless transition: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)));
    QVERIFY(!player.nextSource().isValid());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    player.stop();
}

void TestE2E::testFftFramesDuringGaplessTransition()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.m4a"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    DragonSpectrumAnalyzer analyzer(&player);
    FftFrameCounter counter(&analyzer);

    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-3s-2c-44100hz.m4a"_s);
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesBeforeTransition = counter.count();
    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QTest::qWait(500);
    int framesAfterTransition = counter.count();
    const int underrunsAfter = diagnostics.audioUnderrunCount();

    QVERIFY2(framesBeforeTransition > 0, "FFT frames should arrive during first track playback");
    QVERIFY2(framesAfterTransition > framesBeforeTransition, "FFT frames should continue arriving after gapless transition");
    QVERIFY2(underrunsAfter - underrunsBefore == 0,
             qPrintable(u"Audio should not underrun during gapless transition with FFT: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    player.stop();
}

void TestE2E::testTrackChangeWhilePaused()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.m4a"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    QVERIFY(helper.pauseAndWait());
    VERIFY_PAUSED_STATE(player);
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));
    QVERIFY(sourceSpy.count() > 0);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));
    VERIFY_AUDIO_INACTIVE(diagnostics);

    QVERIFY(helper.playAndWait());
    VERIFY_PLAYING_STATE(player);
    QVERIFY2(stateSpy.count() >= 1, qPrintable(u"Expected state change on resume, got %1"_s.arg(stateSpy.count())));

    qDebug() << "Track change while paused test passed:"
             << "sourceChanged=" << sourceSpy.count() << "stateChanges=" << stateSpy.count();

    player.stop();
}

void TestE2E::testGaplessGenerationCheck()
{
    QStringList shortTracks = TestFixture::shortFixtures();
    QVERIFY(!shortTracks.isEmpty());

    VERIFY_FIXTURE_EXISTS(shortTracks[0]);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-1c-44100hz.flac"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QList<QUrl> sourceHistory;
    connect(
        &player,
        &DragonPlayer::sourceChanged,
        &player,
        [&]() {
            sourceHistory.append(player.source());
        },
        Qt::DirectConnection);

    QVERIFY(helper.setSourceAndWait(shortTracks[0]));
    helper.setNextSource(u"gs-3s-2c-44100hz.ogg"_s);
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(player.position() > 2000, 10000);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)));

    QVERIFY(helper.setSourceAndWait(u"gs-3s-1c-44100hz.flac"_s));
    QTest::qWait(1000);

    QVERIFY2(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)),
             qPrintable(u"Final source should be flac, got %1"_s.arg(player.source().toString())));

    QVERIFY(player.playbackState() != DragonPlayer::PlaybackState::PlayingState || player.status() == DragonPlayer::MediaStatus::LoadedMedia);

    bool track2WasEverSource = sourceHistory.contains(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    if (track2WasEverSource && sourceHistory.last() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s))) {
        qDebug() << "setSource(flac) interrupted an active gapless transition to ogg";
        QVERIFY2(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)),
                 "After interrupting gapless transition, source should be the interrupting file");
    } else if (!track2WasEverSource) {
        qDebug() << "Generation-check path: stale gapless callback to ogg was discarded";
        QVERIFY2(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)),
                 "After discarding stale gapless callback, source should be the explicitly set file");
    }

    player.stop();
}

void TestE2E::testNonGaplessEofWithFftOn()
{
    QStringList shortTracks = TestFixture::shortFixtures();
    QVERIFY(!shortTracks.isEmpty());
    VERIFY_FIXTURE_EXISTS(shortTracks[0]);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);
    DragonSpectrumAnalyzer analyzer(&player);
    FftFrameCounter counter(&analyzer);

    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);
    QVERIFY(helper.setSourceAndWait(shortTracks[0]));
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesDuringPlayback = counter.count();
    QVERIFY2(framesDuringPlayback > 0, "FFT frames should arrive during playback");

    QVERIFY(helper.waitForEndOfMedia());
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QTest::qWait(1000);
    int framesAfterDrain = counter.count();
    int inFlightFrames = framesAfterDrain - framesDuringPlayback;

    QTest::qWait(500);
    int framesAfterCheck = counter.count();
    int genuinelyNewFrames = framesAfterCheck - framesAfterDrain;

    QVERIFY2(genuinelyNewFrames == 0, qPrintable(u"FFT should stop producing frames after EOF; got %1 genuinely new frames"_s.arg(genuinelyNewFrames)));
    qDebug() << "Non-gapless EOF FFT: in-flight frames after drain=" << inFlightFrames << "genuinely new frames=" << genuinelyNewFrames;

    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesAfterRestart = counter.count();

    QVERIFY2(framesAfterRestart > framesAfterDrain, "FFT frames should resume after play() at EndOfMedia");

    player.stop();
}

void TestE2E::testSameFormatSeamlessTransition()
{
    QStringList stereo = TestFixture::stereoFixtures();
    QVERIFY(stereo.size() >= 2);

    VERIFY_FIXTURE_EXISTS(stereo[0]);
    VERIFY_FIXTURE_EXISTS(stereo[1]);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(helper.setSourceAndWait(stereo[0]));
    helper.setNextSource(stereo[1]);
    QVERIFY(player.nextSource().isValid());

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QTest::qWait(1500);
    const int underrunsAfter = diagnostics.audioUnderrunCount();

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during same-format seamless transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(stereo[1])));
    QVERIFY(!player.nextSource().isValid());

    QVERIFY2(underrunsAfter - underrunsBefore == 0, "Audio should not underrun during same-format gapless transition");

    player.stop();
}

void TestE2E::testDiagnosticsBasicFunctionality()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY2(diagnostics.audioBufferUs() == -1, "Audio buffer should return -1 when stopped (no device)");
    QVERIFY2(diagnostics.audioBufferFrames() == -1, "Audio buffer frames should return -1 when stopped (no device)");
    QVERIFY2(diagnostics.decodeQueueSize() == 0, "Decode queue should be 0 when stopped");
    QVERIFY2(diagnostics.fftQueueSize() == 0, "FFT queue should be 0 when stopped");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder when stopped");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active when stopped");

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY2(diagnostics.audioBufferUs() == -1, "Audio buffer should return -1 before playback starts (no device yet)");

    QVERIFY(helper.playAndWait());
    QTest::qWait(100);

    VERIFY_AUDIO_ACTIVE(diagnostics);
    QVERIFY2(diagnostics.audioBufferFrames() >= 0, "Audio buffer should report >=0 frames during playback");
    QVERIFY2(diagnostics.audioBufferUs() >= 0, "Audio buffer should report >=0 µs during playback");
    QVERIFY2(diagnostics.hasActiveDecoder(), "Should have active decoder during playback");
    QVERIFY2(diagnostics.decodeQueueSize() > 0, "Decode queue should have samples during playback");

    QVERIFY(helper.stopAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.audioBufferUs() == -1, 1000);
    QVERIFY2(diagnostics.audioBufferFrames() == -1, "Audio buffer frames should return -1 after stop");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder after stop");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active after stop");

    qDebug() << "Diagnostics basic functionality test passed";
}

void TestE2E::testGaplessTransitionCoroutine()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.m4a"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-3s-2c-44100hz.m4a"_s);

    QVERIFY(player.nextSource().isValid());
    QVERIFY(player.nextSource() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during coroutine-based gapless transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));
    QVERIFY(!player.nextSource().isValid());

    QVERIFY2(trackSpy.count() >= 1, qPrintable(u"Expected trackChanged signal, got %1"_s.arg(trackSpy.count())));

    VERIFY_AUDIO_ACTIVE(diagnostics);
    const int underrunsAfter = diagnostics.audioUnderrunCount();
    QVERIFY2(underrunsAfter - underrunsBefore == 0,
             qPrintable(u"Audio should not underrun during coroutine gapless transition: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    qDebug() << "Coroutine-based gapless transition test passed:"
             << "trackChanged=" << trackSpy.count() << "sourceChanged=" << sourceSpy.count();

    player.stop();
}

void TestE2E::testGaplessFormatMismatch()
{
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-1c-44100hz.flac"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-3s-1c-44100hz.flac"_s);

    QVERIFY(player.nextSource().isValid());
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during gapless format mismatch transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during gapless format mismatch transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-1c-44100hz.flac"_s)));
    QVERIFY(!player.nextSource().isValid());

    QVERIFY(player.duration() > 0);
    VERIFY_AUDIO_ACTIVE(diagnostics);

    const int underrunsAfter = diagnostics.audioUnderrunCount();
    QVERIFY2(
        underrunsAfter - underrunsBefore == 0,
        qPrintable(u"Audio should not underrun during gapless format mismatch transition: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    player.stop();
}

void TestE2E::testGaplessPreWarmError()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto errorSpy = SignalSpyHelper::errorSpy(&player);
    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QUrl firstSource = player.source();

    helper.setNextSource(u"/nonexistent/invalid_file.mp3"_s);

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0 || player.error() != DragonPlayer::Error::NoError, 15000);

    QVERIFY2(sourceSpy.count() >= 2, "Pre-warm failure setSource must emit sourceChanged");

    QVERIFY(player.error() != DragonPlayer::Error::NoError);

    qDebug() << "Gapless pre-warm error test passed:"
             << "errors=" << errorSpy.count() << "finalError=" << static_cast<int>(player.error()) << "finalState=" << static_cast<int>(player.playbackState())
             << "sourceChanges=" << sourceSpy.count();

    player.stop();
}

void TestE2E::testGaplessStarvation()
{
    qputenv("DRAGON_TEST_SLOW_OPEN", "1");

    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-3s-2c-44100hz.m4a"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-3s-2c-44100hz.ogg"_s));

    helper.setNextSource(u"gs-3s-2c-44100hz.m4a"_s);

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    QTest::qWait(1000);

    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QVERIFY(helper.waitForTrackChange());

    QTest::qWait(1500);

    const int underrunsAfter = diagnostics.audioUnderrunCount();

    qDebug() << "Underruns before:" << underrunsBefore << "after:" << underrunsAfter << "delta:" << underrunsAfter - underrunsBefore;

    qunsetenv("DRAGON_TEST_SLOW_OPEN");

    player.stop();

    QCOMPARE(underrunsAfter - underrunsBefore, 0);
}

QTEST_MAIN(TestE2E)
#include "test_e2e.moc"
