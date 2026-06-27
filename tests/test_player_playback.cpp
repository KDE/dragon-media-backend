/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Playback state-transition tests for DragonPlayer.
 * 
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMultimedia/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayerPlayback : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPositionTimerEmitsDuringPlayback();
    void testSeekWithRealAudio();

    void testSetSourceDoesNotEmitPlayingState();
    void testSetSourceDoesNotAutoStartAudio();
    void testSetSourceWhilePlayingEmitsStoppedState();
    void testPlayWithNoSourceIsNoOp();

    void testPlayDuringLoadingDefers();
    void testPauseDuringLoadingDefers();
    void testStopDuringLoadingDefers();
    void testDeferredStateResetOnNewSource();

    void testPauseFromPlayingState();
    void testStopFromPlayingStateEmitsLoadedMedia();
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
    void testSignalOrderOnStop();

    void testDeferredPlayIntentDuringFormatResolution();
    void testSetSourceThenPlayFirstTrack();
    void testPlayNextTrackWhilePlaying();
    void testPlayRapidNextNext();

    void testDeferredPlayAfterFormatReady();

private:
    void skipIfMissing(const QString &filename)
    {
        if (!QFileInfo::exists(TestFixture::fixturePath(filename))) {
            QSKIP(qPrintable(u"Fixture not available: %1"_s.arg(filename)));
        }
    }

    void skipIfMissing(const QStringList &filenames)
    {
        for (const auto &f : filenames) {
            if (!QFileInfo::exists(TestFixture::fixturePath(f))) {
                QSKIP(qPrintable(u"Fixture not available: %1"_s.arg(f)));
            }
        }
    }
};

void TestPlayerPlayback::testPositionTimerEmitsDuringPlayback()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
    QTest::qWait(300);

    QVERIFY2(posSpy.count() > 0, "positionChanged must be emitted during playback");
    QVERIFY2(player.position() >= 0, "position() must return non-negative value");

    player.stop();
}

void TestPlayerPlayback::testSeekWithRealAudio()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(200);
    player.seek(1000);
    QTest::qWait(200);

    VERIFY_POSITION_NEAR(player.position(), 1000, 500);

    player.stop();
}

void TestPlayerPlayback::testSetSourceDoesNotEmitPlayingState()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open without play()");
}

void TestPlayerPlayback::testSetSourceDoesNotAutoStartAudio()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open after setSource()");
}

void TestPlayerPlayback::testSetSourceWhilePlayingEmitsStoppedState()
{
    skipIfMissing(u"sample-3s.mp3"_s);
    skipIfMissing(u"gs-16b-2c-44100hz.ogg"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
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
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.play();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPlayDuringLoadingDefers()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

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
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

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
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 0);

    player.stop();
    QCOMPARE(stateSpy.count(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open after stop() during loading");
}

void TestPlayerPlayback::testDeferredStateResetOnNewSource()
{
    skipIfMissing(u"sample-3s.mp3"_s);

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
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.pause();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PausedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    QVERIFY2(player.isAudioActive(), "Audio should stay open after pause");

    player.stop();
}

void TestPlayerPlayback::testStopFromPlayingStateEmitsLoadedMedia()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.stop();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio should be closed after stop()");

    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia),
             "stop() must NOT emit LoadedMedia if already LoadedMedia per Qt dedup");
}

void TestPlayerPlayback::testPlayFromPausedStateResumes()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(300);
    int64_t posBefore = player.position();

    QVERIFY(helper.pauseAndWait());
    QTest::qWait(200);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);

    QVERIFY2(player.position() >= posBefore, qPrintable(u"Position should not reset: was %1, now %2"_s.arg(posBefore).arg(player.position())));

    player.stop();
}

void TestPlayerPlayback::testMultiplePlayCallsIdempotent()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();
    player.play();
    player.play();
    QTest::qWait(100);

    QCOMPARE(stateSpy.count(), 0);
    player.stop();
}

void TestPlayerPlayback::testPlayAfterStopRestartsDecoder()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(300);

    QSignalSpy stoppedSpy(&player, &DragonPlayer::stopped);
    player.stop();
    QTRY_VERIFY_WITH_TIMEOUT(stoppedSpy.count() >= 1, 3000);
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy playingSpy(&player, &DragonPlayer::playing);
    player.play();

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(playingSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::PlayingState), "Must transition to PlayingState after play()");

    player.stop();
}

void TestPlayerPlayback::testSetSourceWhilePlayingStopsOldTrack()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::StoppedState), "setSource() while playing must emit StoppedState");

    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia),
             "setSource(sameUrl) while playing must NOT emit LoadedMedia from implicit stop (dedup)");
    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia),
             "setSource(sameUrl) must NOT emit LoadingMedia (early return)");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testEndOfMediaTransitionsToStoppedState()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);

    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::StoppedState), "End of media must emit StoppedState");

    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::EndOfMedia), "End of media must emit EndOfMedia status");

    QTRY_COMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerPlayback::testPlayAtEndOfMediaRestarts()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);
    QTRY_COMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    VERIFY_POSITION_NEAR(player.position(), 0, 500);

    player.stop();
}

void TestPlayerPlayback::testPlayDuringLoadingStartsAudioOnComplete()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.play();

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);

    QVERIFY2(player.isAudioActive(), "play() during LoadingMedia must start audio when init completes");

    player.stop();
}

void TestPlayerPlayback::testStopDuringLoadingPreventsAudioStart()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.stop();

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "stop() during LoadingMedia must prevent audio from starting");
}

void TestPlayerPlayback::testPlayThenStopDuringLoadingCancelsStart()
{
    skipIfMissing(u"sample-3s.mp3"_s);
    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.play();
    player.stop();

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "stop() overriding play() during LoadingMedia must cancel audio start");
}

void TestPlayerPlayback::testSignalOrderOnSetSource()
{
    skipIfMissing(u"sample-3s.mp3"_s);

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
void TestPlayerPlayback::testSignalOrderOnStop()
{
    skipIfMissing(u"sample-3s.mp3"_s);

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

    QVERIFY2(tracker.contains(u"stateChanged(StoppedState)"_s), "stop() must emit StoppedState");
    QVERIFY2(!tracker.contains(u"statusChanged(LoadedMedia)"_s), "stop() must NOT emit LoadedMedia (dedup)");
}

void TestPlayerPlayback::testDeferredPlayIntentDuringFormatResolution()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.play();

    QVERIFY(helper.waitForState(DragonPlayer::PlaybackState::PlayingState, 10000));
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);
    QVERIFY(player.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testSetSourceThenPlayFirstTrack()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QVERIFY(helper.playAndWait());
    QVERIFY(player.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testPlayNextTrackWhilePlaying()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QVERIFY(player.isAudioActive());

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();

    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY(player.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testPlayRapidNextNext()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s, u"sample-3s.aac"_s});

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();

    player.stop();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.aac"_s)));
    player.play();

    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY(player.isAudioActive());

    player.stop();
}

void TestPlayerPlayback::testDeferredPlayAfterFormatReady()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.play();

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY(player.status() == DragonPlayer::MediaStatus::LoadedMedia || player.status() == DragonPlayer::MediaStatus::BufferingMedia);
    QVERIFY2(player.isAudioActive(), "Deferred play must start audio after format ready");
}

QTEST_MAIN(TestPlayerPlayback)
#include "test_player_playback.moc"
