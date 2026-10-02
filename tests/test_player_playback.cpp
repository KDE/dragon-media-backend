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
#include <DragonPlayer>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{
std::chrono::milliseconds maxBackwardStep(const QSignalSpy &spy)
{
    std::chrono::milliseconds maxBackward{0};
    std::optional<std::chrono::milliseconds> prev;
    for (const auto &args : spy) {
        const std::chrono::milliseconds pos = args.at(0).value<std::chrono::milliseconds>();
        if (prev) {
            maxBackward = std::max(maxBackward, *prev - pos);
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
    void testStopDuringLoadingThenPlayPlays();

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
    QVERIFY2(player.position() > 0ms, "position() must have advanced from 0 during playback");

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
    player.setPosition(1000ms);

    VERIFY_POSITION_NEAR(player.position(), 1000ms, 50ms);

    QTRY_VERIFY_WITH_TIMEOUT(diag.audioPosition() >= 1000ms, 5000);

    QTest::qWait(700);
    QVERIFY2(player.position() > 1000ms, "Position must keep advancing after seek");

    VERIFY_POSITION_NEAR(player.position(), diag.audioPosition(), 250ms);

    const std::chrono::milliseconds backward = maxBackwardStep(posSpy);
    QVERIFY2(backward < 300ms, qPrintable(u"Position jumped backwards by %1ms after seek"_s.arg(backward.count())));

    player.stop();
}

void TestPlayerPlayback::testSeekNearEndSnapsToDuration()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    const std::chrono::milliseconds duration = player.duration().value_or(0ms);
    QVERIFY2(duration > 2000ms, qPrintable(u"Fixture must be longer than 2s, got %1ms"_s.arg(duration.count())));

    QTest::qWait(200);

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    QElapsedTimer clock;
    player.setPosition(duration - 1000ms);
    clock.start();

    QVERIFY2(helper.waitForEndOfMedia(10000), "Seeking near the end must still reach EndOfMedia");
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(
        elapsed >= 500 && elapsed < 1800,
        qPrintable(u"EndOfMedia arrived %1ms after seeking to %2ms of a %3ms track"_s.arg(elapsed).arg((duration - 1000ms).count()).arg(duration.count())));

    QCOMPARE(player.position(), duration);
    QVERIFY2(posSpy.count() > 0, "positionChanged must be emitted after seek");
    QCOMPARE(posSpy.last().at(0).value<std::chrono::milliseconds>(), duration);
    for (const auto &args : posSpy) {
        const std::chrono::milliseconds pos = args.at(0).value<std::chrono::milliseconds>();
        QVERIFY2(pos <= duration + 50ms, qPrintable(u"Position %1ms exceeded duration %2ms"_s.arg(pos.count()).arg(duration.count())));
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
    const std::chrono::milliseconds pausedPos = player.position();
    QVERIFY2(pausedPos > 0ms, "Position must have advanced before pause");

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    QTest::qWait(400);
    QCOMPARE(posSpy.count(), 0);
    QCOMPARE(player.position(), pausedPos);

    QVERIFY(helper.playAndWait());
    QTest::qWait(300);
    QVERIFY2(player.position() > pausedPos, "Position must resume advancing after play");
    QVERIFY2(player.position() < pausedPos + 1000ms,
             qPrintable(u"Position must not leap forward after resume: paused at %1ms, now %2ms"_s.arg(pausedPos.count()).arg(player.position().count())));

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

    player.setPosition(1500ms);
    VERIFY_POSITION_NEAR(player.position(), 1500ms, 50ms);

    QTest::qWait(300);
    VERIFY_POSITION_NEAR(player.position(), 1500ms, 50ms);

    QVERIFY(helper.playAndWait());
    QTest::qWait(700);
    QVERIFY2(player.position() >= 1500ms, qPrintable(u"Position must resume from the seek target, got %1ms"_s.arg(player.position().count())));
    QVERIFY2(player.position() < 2500ms, qPrintable(u"Position must not leap forward after resume, got %1ms"_s.arg(player.position().count())));
    VERIFY_POSITION_NEAR(player.position(), diag.audioPosition(), 250ms);

    player.stop();
}

void TestPlayerPlayback::testSetPositionClampedToDuration()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY2(player.duration(), "Duration must be known after load");
    const std::chrono::milliseconds duration = *player.duration();

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(duration + 5000ms);
    QCOMPARE(player.position(), duration);
    QCOMPARE(posSpy.last().at(0).value<std::chrono::milliseconds>(), duration);

    player.setPosition(-500ms);
    QCOMPARE(player.position(), 0ms);
    QCOMPARE(posSpy.last().at(0).value<std::chrono::milliseconds>(), 0ms);

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

    QVERIFY2(player.position() < 700ms, qPrintable(u"Position must reset at gapless transition, got %1ms"_s.arg(player.position().count())));

    const std::chrono::milliseconds afterChange = player.position();
    QTest::qWait(800);
    QVERIFY2(player.position() > afterChange, "Position must advance on the new track");
    VERIFY_POSITION_NEAR(player.position(), diag.audioPosition(), 250ms);

    player.stop();
}

void TestPlayerPlayback::testStopResetsPositionToZero()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(300);
    QVERIFY2(player.position() > 0ms, "Position must have advanced before stop");

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    player.stop();
    QCOMPARE(player.position(), 0ms);
    QVERIFY2(posSpy.count() > 0, "stop() must emit positionChanged(0)");
    QCOMPARE(posSpy.last().at(0).value<std::chrono::milliseconds>(), 0ms);

    const int spyCountAfterStop = posSpy.count();
    player.stop();
    QCOMPARE(player.position(), 0ms);
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
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadingMedia);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(player.duration().value_or(0ms) > 0ms, "stop() must not cancel an in-flight load; the load must complete for real");
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
    const std::chrono::milliseconds posBefore = player.position();

    QVERIFY(helper.pauseAndWait());
    QTest::qWait(200);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.play();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);

    QVERIFY2(player.position() >= posBefore, qPrintable(u"Position should not reset: was %1, now %2"_s.arg(posBefore.count()).arg(player.position().count())));

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
    VERIFY_POSITION_NEAR(player.position(), 0ms, 200ms);

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
    QVERIFY2(player.duration().value_or(0ms) > 0ms, "stop() must not cancel the in-flight load; status must come from a real load");
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
    QVERIFY2(player.duration().value_or(0ms) > 0ms, "stop() must not cancel the in-flight load; status must come from a real load");
    QVERIFY2(!diag.isAudioActive(), "stop() overriding play() during LoadingMedia must cancel audio start");
}

void TestPlayerPlayback::testStopDuringLoadingThenPlayPlays()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadingMedia);

    player.stop();
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadingMedia);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QVERIFY2(player.duration().value_or(0ms) > 0ms, "load must have completed for real, not been cancelled by stop()");

    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(diag.isAudioActive(), "play() after stop-during-loading must start audio from the completed load");

    player.stop();
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
