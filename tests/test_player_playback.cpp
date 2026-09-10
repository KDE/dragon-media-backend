/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Playback state-transition tests for DragonPlayer.
 *
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "player/dragondiagnostics.h"
#include <DragonMediaBackend/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace
{
qint64 maxBackwardStep(const QSignalSpy &spy)
{
    qint64 maxBackward = 0;
    qint64 prev = -1;
    for (const auto &args : spy) {
        const qint64 pos = args.at(0).toLongLong();
        if (prev >= 0) {
            maxBackward = std::max(maxBackward, prev - pos);
        }
        prev = pos;
    }
    return maxBackward;
}
}

class TestPlayerPlayback : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPositionTimerEmitsDuringPlayback();
    void testSeekWithRealAudio();
    void testSeekNearEndSnapsToDuration();
    void testPositionFrozenWhilePaused();
    void testSetPositionWhilePausedThenResume();
    void testSetPositionClampedToDuration();
    void testPositionResetsOnGaplessTransition();
    void testStopResetsPositionToZero();

    void testSetSourceDoesNotEmitPlayingState();
    void testSetSourceWhilePlayingEmitsStoppedState();
    void testPlayWithNoSourceIsNoOp();

    void testPlayDuringLoadingDefers();
    void testPauseDuringLoadingDefers();
    void testStopDuringLoadingDefers();
    void testDeferredStateResetOnNewSource();

    void testPauseFromPlayingState();
    void testStopFromPlayingStateDoesNotEmitLoadedMedia();
    void testPlayFromPausedStateResumes();
    void testMultiplePlayCallsIdempotent();
    void testPlayAfterStopRestartsDecoder();

    void testSetSourceWhilePlayingStopsOldTrack();
    void testEndOfMediaTransitionsToStoppedState();
    void testPlayAtEndOfMediaRestarts();

    void testPlayDuringLoadingStartsAudioOnComplete();
    void testStopDuringLoadingPreventsAudioStart();
    void testPlayThenStopDuringLoadingCancelsStart();

    void testSignalOrderOnSetSource();
    void testSignalPresenceOnStop();

    void testDeferredPlayIntentDuringFormatResolution();
    void testSetSourceThenPlayFirstTrack();
    void testPlayNextTrackAfterStop();
    void testPlayRapidNextNext();
};

void TestPlayerPlayback::testPositionTimerEmitsDuringPlayback()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
    QTest::qWait(300);

    QVERIFY2(posSpy.count() > 0, "positionChanged must be emitted during playback");
    QVERIFY2(player.position() > 0, "position() must have advanced from 0 during playback");

    player.stop();
}

void TestPlayerPlayback::testSeekWithRealAudio()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(200);

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    player.setPosition(1000);

    VERIFY_POSITION_NEAR(player.position(), 1000, 50);

    QTRY_VERIFY_WITH_TIMEOUT(diag.audioPositionMs() >= 1000, 5000);

    QTest::qWait(700);
    QVERIFY2(player.position() > 1000, "Position must keep advancing after seek");

    VERIFY_POSITION_NEAR(player.position(), diag.audioPositionMs(), 250);

    const qint64 backward = maxBackwardStep(posSpy);
    QVERIFY2(backward < 300, qPrintable(u"Position jumped backwards by %1ms after seek"_s.arg(backward)));

    player.stop();
}

void TestPlayerPlayback::testSeekNearEndSnapsToDuration()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    const qint64 duration = player.duration();
    QVERIFY2(duration > 2000, qPrintable(u"Fixture must be longer than 2s, got %1ms"_s.arg(duration)));

    QTest::qWait(200);

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    QElapsedTimer clock;
    player.setPosition(duration - 1000);
    clock.start();

    QVERIFY2(helper.waitForEndOfMedia(10000), "Seeking near the end must still reach EndOfMedia");
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(elapsed >= 500 && elapsed < 1800,
             qPrintable(u"EndOfMedia arrived %1ms after seeking to %2ms of a %3ms track"_s.arg(elapsed).arg(duration - 1000).arg(duration)));

    QCOMPARE(player.position(), duration);
    QVERIFY2(posSpy.count() > 0, "positionChanged must be emitted after seek");
    QCOMPARE(posSpy.last().at(0).toLongLong(), duration);
    for (const auto &args : posSpy) {
        const qint64 pos = args.at(0).toLongLong();
        QVERIFY2(pos <= duration + 50, qPrintable(u"Position %1ms exceeded duration %2ms"_s.arg(pos).arg(duration)));
    }

    player.stop();
}

void TestPlayerPlayback::testPositionFrozenWhilePaused()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(300);

    QVERIFY(helper.pauseAndWait());
    const qint64 pausedPos = player.position();
    QVERIFY2(pausedPos > 0, "Position must have advanced before pause");

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    QTest::qWait(400);
    QCOMPARE(posSpy.count(), 0);
    QCOMPARE(player.position(), pausedPos);

    QVERIFY(helper.playAndWait());
    QTest::qWait(300);
    QVERIFY2(player.position() > pausedPos, "Position must resume advancing after play");
    QVERIFY2(player.position() < pausedPos + 1000,
             qPrintable(u"Position must not leap forward after resume: paused at %1ms, now %2ms"_s.arg(pausedPos).arg(player.position())));

    player.stop();
}

void TestPlayerPlayback::testSetPositionWhilePausedThenResume()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(200);
    QVERIFY(helper.pauseAndWait());

    player.setPosition(1500);
    VERIFY_POSITION_NEAR(player.position(), 1500, 50);

    QTest::qWait(300);
    VERIFY_POSITION_NEAR(player.position(), 1500, 50);

    QVERIFY(helper.playAndWait());
    QTest::qWait(700);
    QVERIFY2(player.position() >= 1500, qPrintable(u"Position must resume from the seek target, got %1ms"_s.arg(player.position())));
    QVERIFY2(player.position() < 2500, qPrintable(u"Position must not leap forward after resume, got %1ms"_s.arg(player.position())));
    VERIFY_POSITION_NEAR(player.position(), diag.audioPositionMs(), 250);

    player.stop();
}

void TestPlayerPlayback::testSetPositionClampedToDuration()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    const qint64 duration = player.duration();
    QVERIFY2(duration > 0, "Duration must be known after load");

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(duration + 5000);
    QCOMPARE(player.position(), duration);
    QCOMPARE(posSpy.last().at(0).toLongLong(), duration);

    player.setPosition(-500);
    QCOMPARE(player.position(), 0LL);
    QCOMPARE(posSpy.last().at(0).toLongLong(), 0LL);

    player.stop();
}

void TestPlayerPlayback::testPositionResetsOnGaplessTransition()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    helper.setNextSource(u"gs-3s-2c-44100hz.ogg"_s);
    QVERIFY(helper.playAndWait());

    QVERIFY2(helper.waitForTrackChange(15000), "Gapless transition must occur");

    QVERIFY2(player.position() < 700, qPrintable(u"Position must reset at gapless transition, got %1ms"_s.arg(player.position())));

    const qint64 afterChange = player.position();
    QTest::qWait(800);
    QVERIFY2(player.position() > afterChange, "Position must advance on the new track");
    VERIFY_POSITION_NEAR(player.position(), diag.audioPositionMs(), 250);

    player.stop();
}

void TestPlayerPlayback::testStopResetsPositionToZero()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(300);
    QVERIFY2(player.position() > 0, "Position must have advanced before stop");

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    player.stop();
    QCOMPARE(player.position(), 0LL);
    QVERIFY2(posSpy.count() > 0, "stop() must emit positionChanged(0)");
    QCOMPARE(posSpy.last().at(0).toLongLong(), 0LL);

    const int spyCountAfterStop = posSpy.count();
    player.stop();
    QCOMPARE(player.position(), 0LL);
    QCOMPARE(posSpy.count(), spyCountAfterStop);
}

void TestPlayerPlayback::testSetSourceDoesNotEmitPlayingState()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!diag.isAudioActive(), "Audio must NOT be open without play()");
}

void TestPlayerPlayback::testSetSourceWhilePlayingEmitsStoppedState()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    auto firstState = stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>();
    QCOMPARE(firstState, DragonPlayer::PlaybackState::StoppedState);

    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia), "setSource() must emit LoadingMedia status");

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPlayWithNoSourceIsNoOp()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    player.play();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPlayDuringLoadingDefers()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 0);

    player.play();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QTRY_VERIFY(stateSpy.count() == 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
}

void TestPlayerPlayback::testPauseDuringLoadingDefers()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 0);

    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QTRY_VERIFY(stateSpy.count() == 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PausedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
}

void TestPlayerPlayback::testStopDuringLoadingDefers()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 0);

    player.stop();
    QCOMPARE(stateSpy.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!diag.isAudioActive(), "Audio must NOT be open after stop() during loading");
}

void TestPlayerPlayback::testDeferredStateResetOnNewSource()
{
    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.play();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPauseFromPlayingState()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.pause();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PausedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    QVERIFY2(diag.isAudioActive(), "Audio should stay open after pause");

    player.stop();
}

void TestPlayerPlayback::testStopFromPlayingStateDoesNotEmitLoadedMedia()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.stop();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!diag.isAudioActive(), "Audio should be closed after stop()");

    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia),
             "stop() must NOT emit LoadedMedia if already LoadedMedia per Qt dedup");
}

void TestPlayerPlayback::testPlayFromPausedStateResumes()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(300);
    qint64 posBefore = player.position();

    QVERIFY(helper.pauseAndWait());
    QTest::qWait(200);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.play();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);

    QVERIFY2(player.position() >= posBefore, qPrintable(u"Position should not reset: was %1, now %2"_s.arg(posBefore).arg(player.position())));

    player.stop();
}

void TestPlayerPlayback::testMultiplePlayCallsIdempotent()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.play();
    player.play();
    player.play();
    QTest::qWait(100);

    QCOMPARE(stateSpy.count(), 0);
    player.stop();
}

void TestPlayerPlayback::testPlayAfterStopRestartsDecoder()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(300);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.stop();
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 3000);
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.play();

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsTransition(stateSpy, DragonPlayer::PlaybackState::PlayingState, DragonPlayer::PlaybackState::StoppedState),
             "Must transition from StoppedState to PlayingState after play()");

    player.stop();
}

void TestPlayerPlayback::testSetSourceWhilePlayingStopsOldTrack()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsTransition(stateSpy, DragonPlayer::PlaybackState::StoppedState, DragonPlayer::PlaybackState::PlayingState),
             "setSource() while playing must emit stateChanged(StoppedState, PlayingState)");

    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia),
             "setSource(sameUrl) while playing must NOT emit LoadedMedia from implicit stop (dedup)");
    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia),
             "setSource(sameUrl) must NOT emit LoadingMedia (early return)");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testEndOfMediaTransitionsToStoppedState()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 10000);

    QVERIFY2(SignalSpyHelper::containsTransition(stateSpy, DragonPlayer::PlaybackState::StoppedState, DragonPlayer::PlaybackState::PlayingState),
             "End of media must emit stateChanged(StoppedState, PlayingState)");
    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::EndOfMedia), "End of media must emit EndOfMedia status");

    QTRY_COMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPlayAtEndOfMediaRestarts()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);
    QTRY_COMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.play();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    VERIFY_POSITION_NEAR(player.position(), 0, 200);

    player.stop();
}

void TestPlayerPlayback::testPlayDuringLoadingStartsAudioOnComplete()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.play();

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);

    QVERIFY2(diag.isAudioActive(), "play() during LoadingMedia must start audio when init completes");

    player.stop();
}

void TestPlayerPlayback::testStopDuringLoadingPreventsAudioStart()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.stop();

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!diag.isAudioActive(), "stop() during LoadingMedia must prevent audio from starting");
}

void TestPlayerPlayback::testPlayThenStopDuringLoadingCancelsStart()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.play();
    player.stop();

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!diag.isAudioActive(), "stop() overriding play() during LoadingMedia must cancel audio start");
}

void TestPlayerPlayback::testSignalOrderOnSetSource()
{
    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackStateChanges();
    tracker.trackStatusChanges();
    tracker.trackSourceChanges();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s), "setSource() must emit LoadingMedia");
    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource() must emit sourceChanged");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"sourceChanged()"_s), "LoadingMedia must come before sourceChanged");
}
void TestPlayerPlayback::testSignalPresenceOnStop()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    SignalOrderTracker tracker(&player);
    tracker.trackStateChanges();
    tracker.trackStatusChanges();
    tracker.trackPositionZero();

    player.stop();
    QTRY_VERIFY(tracker.events().count() >= 1);

    QVERIFY2(tracker.containsTransition(DragonPlayer::PlaybackState::StoppedState, DragonPlayer::PlaybackState::PlayingState),
             "stop() must emit stateChanged(StoppedState, PlayingState)");
    QVERIFY2(!tracker.contains(u"statusChanged(LoadedMedia)"_s), "stop() must NOT emit LoadedMedia (dedup)");
}

void TestPlayerPlayback::testDeferredPlayIntentDuringFormatResolution()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.play();

    QVERIFY(helper.waitForState(DragonPlayer::PlaybackState::PlayingState, 10000));
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);
    QVERIFY(diag.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testSetSourceThenPlayFirstTrack()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QVERIFY(helper.playAndWait());
    QVERIFY(diag.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testPlayNextTrackAfterStop()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QVERIFY(diag.isAudioActive());

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    player.play();

    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY(diag.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testPlayRapidNextNext()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QVERIFY2(diag.isAudioActive(), "Audio should be active during first track (mp3)");

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(diag.isAudioActive(), "Audio should be active during second track (ogg)");

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.aac"_s)));
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(diag.isAudioActive(), "Audio should be active during third track (aac)");

    player.stop();
}

QTEST_MAIN(TestPlayerPlayback)
#include "test_player_playback.moc"
