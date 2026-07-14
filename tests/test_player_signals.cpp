/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Signal-ordering and Qt Multimedia state-machine compliance tests.
 * Verifies exact signal sequences, deduplication rules, and cross-signal ordering.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMultimedia/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayerSignals : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPlayingChangedSignal();
    void testMediaStatusChangedDedupOnRedundantStop();
    void testSignalOrderOnPlay();
    void testSignalOrderOnPause();
    void testSignalOrderOnEndOfMedia();
    void testDurationChangedAfterLoadedMedia();
    void testSourceChangedFirstInSetSource();
    void testSetSourceSameUrlStopsFirst();
    void testPauseFromStoppedIsNoOp();
    void testNextWhilePlayingWithoutExplicitStop();
    void testSetPositionZeroAtEndOfMedia();
    void testMultipleStopIdempotent();
    void testErrorChangedBeforeInvalidMedia();

    void testRapidSetSourceOnlyLastProcessed();
    void testSetSourceInterruptedByStop();

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

void TestPlayerSignals::testPlayingChangedSignal()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    QSignalSpy playingSpy(&player, &DragonPlayer::playingChanged);
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(playingSpy.count() >= 1, "playingChanged(true) must be emitted on transition to PlayingState");
    QVERIFY2(playingSpy.at(0).at(0).toBool() == true, "playingChanged must emit true");

    playingSpy.clear();
    player.pause();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PausedState, 5000);
    QVERIFY2(playingSpy.count() >= 1, "playingChanged(false) must be emitted on transition from PlayingState");
    QVERIFY2(playingSpy.at(0).at(0).toBool() == false, "playingChanged must emit false");

    playingSpy.clear();
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(playingSpy.count() >= 1, "playingChanged(true) must be emitted on transition to PlayingState");

    playingSpy.clear();
    player.stop();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 5000);
    QVERIFY2(playingSpy.count() >= 1, "playingChanged(false) must be emitted on transition from PlayingState");

    playingSpy.clear();
    player.stop();
    QTest::qWait(100);
    QCOMPARE(playingSpy.count(), 0);
}

void TestPlayerSignals::testMediaStatusChangedDedupOnRedundantStop()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);

    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.stop();
    QTest::qWait(200);
    player.stop();
    QTest::qWait(200);

    int loadedMediaCount = 0;
    for (const auto &args : statusSpy) {
        if (args.at(0).template value<DragonPlayer::MediaStatus>() == DragonPlayer::MediaStatus::LoadedMedia) {
            ++loadedMediaCount;
        }
    }
    QVERIFY2(loadedMediaCount == 0, "Multiple stop() from LoadedMedia must emit mediaStatusChanged(LoadedMedia) zero times (Qt dedup)");
}

void TestPlayerSignals::testSignalOrderOnPlay()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    SignalOrderTracker tracker(&player);
    tracker.trackPlayingChanges();
    tracker.trackStateChanges();

    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);

    QVERIFY2(tracker.contains(u"playingChanged(true)"_s), "play() must emit playingChanged(true)");
    QVERIFY2(tracker.contains(u"stateChanged(PlayingState)"_s), "play() must emit playbackStateChanged(PlayingState)");
    QVERIFY2(tracker.verifyOrder(u"playingChanged(true)"_s, u"stateChanged(PlayingState)"_s),
             "playingChanged(true) must precede playbackStateChanged(PlayingState)");
}

void TestPlayerSignals::testSignalOrderOnPause()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    SignalOrderTracker tracker(&player);
    tracker.trackPlayingChanges();
    tracker.trackStateChanges();

    player.pause();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PausedState, 5000);

    QVERIFY2(tracker.contains(u"playingChanged(false)"_s), "pause() must emit playingChanged(false)");
    QVERIFY2(tracker.contains(u"stateChanged(PausedState)"_s), "pause() must emit playbackStateChanged(PausedState)");
    QVERIFY2(tracker.verifyOrder(u"playingChanged(false)"_s, u"stateChanged(PausedState)"_s),
             "playingChanged(false) must precede playbackStateChanged(PausedState)");
}

void TestPlayerSignals::testSignalOrderOnEndOfMedia()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    SignalOrderTracker tracker(&player);
    tracker.trackPositionChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    QTRY_VERIFY_WITH_TIMEOUT(helper.waitForEndOfMedia(15000), 15000);

    QVERIFY2(tracker.containsPrefix(u"positionChanged("_s), "EndOfMedia must emit positionChanged(duration)");
    QVERIFY2(tracker.contains(u"stateChanged(StoppedState)"_s), "EndOfMedia must emit playbackStateChanged(StoppedState)");
    QVERIFY2(tracker.contains(u"statusChanged(EndOfMedia)"_s), "EndOfMedia must emit mediaStatusChanged(EndOfMedia)");

    QVERIFY2(tracker.verifyOrderPrefix(u"positionChanged("_s, u"statusChanged(EndOfMedia)"_s),
             "positionChanged(duration) must precede mediaStatusChanged(EndOfMedia)");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(EndOfMedia)"_s, u"stateChanged(StoppedState)"_s),
             "mediaStatusChanged(EndOfMedia) must precede playbackStateChanged(StoppedState)");
}

void TestPlayerSignals::testDurationChangedAfterLoadedMedia()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackDurationChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QVERIFY(player.duration() > 0);

    QVERIFY2(tracker.containsPrefix(u"durationChanged("_s), "durationChanged must be emitted during load");
    QVERIFY2(tracker.contains(u"statusChanged(LoadedMedia)"_s), "LoadedMedia must be emitted");

    QVERIFY2(tracker.verifyOrderPrefix(u"durationChanged("_s, u"statusChanged(LoadedMedia)"_s), "durationChanged must precede mediaStatusChanged(LoadedMedia)");
}

void TestPlayerSignals::testSourceChangedFirstInSetSource()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackSourceChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource() must emit sourceChanged");

    int sourceIdx = tracker.events().indexOf(u"sourceChanged()"_s);
    int statusIdx = tracker.events().indexOf(u"statusChanged(LoadingMedia)"_s);

    QVERIFY2(sourceIdx >= 0, "sourceChanged must be emitted");
    QVERIFY2(statusIdx >= 0, "statusChanged(LoadingMedia) must be emitted");
    QVERIFY2(statusIdx < sourceIdx, "statusChanged(LoadingMedia) must precede sourceChanged (QM order)");

    QVERIFY2(!tracker.contains(u"stateChanged(StoppedState)"_s), "setSource from StoppedState must NOT force-emit playbackStateChanged(StoppedState)");
}

void TestPlayerSignals::testSetSourceSameUrlStopsFirst()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::StoppedState), "setSource(sameUrl) must emit StoppedState (implicit stop)");

    QVERIFY2(!SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia),
             "setSource(sameUrl) must NOT emit LoadingMedia, per Qt setSource early return");

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
}

void TestPlayerSignals::testPauseFromStoppedIsNoOp()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.pause();

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 0);
}

void TestPlayerSignals::testNextWhilePlayingWithoutExplicitStop()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::StoppedState), "setSource(newUrl) while playing must emit StoppedState");
    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia),
             "setSource(newUrl) while playing must emit LoadedMedia from implicit stop");
    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia), "setSource(newUrl) while playing must emit LoadingMedia");

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
}

void TestPlayerSignals::testSetPositionZeroAtEndOfMedia()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(helper.waitForEndOfMedia(15000), 15000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::EndOfMedia);

    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    player.setPosition(0);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 5000);
    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia), "setPosition(0) at EndOfMedia must emit LoadedMedia");
}

void TestPlayerSignals::testMultipleStopIdempotent()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.stop();
    QTest::qWait(200);

    player.stop();
    QTest::qWait(200);

    QCOMPARE(stateSpy.count(), 1);
    QVERIFY2(SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::StoppedState), "First stop must emit StoppedState");

    int loadedCount = 0;
    for (const auto &args : statusSpy) {
        if (args.at(0).template value<DragonPlayer::MediaStatus>() == DragonPlayer::MediaStatus::LoadedMedia) {
            ++loadedCount;
        }
    }
    QVERIFY2(loadedCount == 0, "Multiple stop() from LoadedMedia must emit mediaStatusChanged(LoadedMedia) zero times (Qt dedup)");
}

void TestPlayerSignals::testErrorChangedBeforeInvalidMedia()
{
    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackErrorChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);

    QVERIFY2(tracker.containsPrefix(u"statusChanged(InvalidMedia)"_s), "InvalidMedia must emit statusChanged(InvalidMedia)");
    QVERIFY2(tracker.containsPrefix(u"errorChanged("_s), "InvalidMedia must emit errorChanged");

    QVERIFY2(tracker.verifyOrderPrefix(u"statusChanged(InvalidMedia)"_s, u"errorChanged("_s),
             "mediaStatusChanged(InvalidMedia) must precede errorChanged per Qt");
}

void TestPlayerSignals::testRapidSetSourceOnlyLastProcessed()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;

    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    const QUrl sourceA = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl sourceB = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s));

    player.setSource(sourceA);
    player.setSource(sourceB);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.source(), sourceB);

    QVERIFY2(sourceSpy.count() >= 2, "Both setSource calls must emit sourceChanged");

    QVERIFY2(player.duration() > 0, "Duration must be positive after loading");
    QVERIFY2(player.source() == sourceB, "Player source must be the last-set source");
}

void TestPlayerSignals::testSetSourceInterruptedByStop()
{
    skipIfMissing(u"sample-3s.mp3"_s);
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.stop();

    QTest::qWait(500);

    QVERIFY2(player.status() == DragonPlayer::MediaStatus::LoadedMedia,
             qPrintable(u"Expected LoadedMedia after stop-during-load completes, got status %1"_s.arg(static_cast<int>(player.status()))));

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

QTEST_MAIN(TestPlayerSignals)
#include "test_player_signals.moc"
