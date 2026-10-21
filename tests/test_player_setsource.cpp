/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests verifying that DragonPlayer::setSource() matches QMediaPlayer's
 * signal ordering: the implicit stop and LoadingMedia transitions must
 * precede sourceChanged, and state must not be force-emitted when
 * already StoppedState.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonPlayer>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayerSetSource : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSetSourceFromFreshPlayerNoForceEmit();
    void testSetSourceSignalOrderFromFreshPlayer();
    void testSetSourceSignalOrderFromPlaying();
    void testSetSourceSameUrlWhileStoppedReloads();
    void testSetSourceThenClearInSameTurnDoesNotLoad();
    void testSetSourceResetsPositionToZeroAndEmitsSignal();
};

void TestPlayerSetSource::testSetSourceFromFreshPlayerNoForceEmit()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 0);
}

void TestPlayerSetSource::testSetSourceSignalOrderFromFreshPlayer()
{
    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackSourceChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource() must emit sourceChanged");
    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s), "setSource() must emit LoadingMedia");

    int loadingIdx = tracker.indexOfPrefix(u"statusChanged(LoadingMedia)"_s);
    int sourceIdx = tracker.events().indexOf(u"sourceChanged()"_s);

    QVERIFY2(loadingIdx < sourceIdx, "statusChanged(LoadingMedia) must precede sourceChanged (QM order)");

    QVERIFY2(!tracker.containsPrefix(u"playbackStateChanged(StoppedState"_s),
             "setSource from StoppedState must NOT force-emit playbackStateChanged(StoppedState)");
}

void TestPlayerSetSource::testSetSourceSignalOrderFromPlaying()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    SignalOrderTracker tracker(&player);
    tracker.trackSourceChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.containsTransition(DragonPlayer::PlaybackState::StoppedState, DragonPlayer::PlaybackState::PlayingState),
             "setSource while playing must emit playbackStateChanged(StoppedState, PlayingState)");
    QVERIFY2(tracker.contains(u"statusChanged(LoadedMedia)"_s), "setSource while playing must emit LoadedMedia from implicit stop");
    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s), "setSource must emit LoadingMedia for the new source");
    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource must emit sourceChanged");

    QVERIFY2(tracker.verifyOrder(u"playbackStateChanged(StoppedState, PlayingState)"_s, u"sourceChanged()"_s),
             "playbackStateChanged(StoppedState, PlayingState) must precede sourceChanged");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"statusChanged(LoadedMedia)"_s),
             "source swap while playing must go LoadingMedia -> LoadedMedia from the real load (no synthetic LoadedMedia from the implicit stop)");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"sourceChanged()"_s), "statusChanged(LoadingMedia) must precede sourceChanged");
}

void TestPlayerSetSource::testSetSourceSameUrlWhileStoppedReloads()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.stopAndWait());
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);

    SignalOrderTracker tracker(&player);
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s),
             "setSource(sameUrl) while stopped with loaded media must run a real reload cycle (LoadingMedia)");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"statusChanged(LoadedMedia)"_s),
             "reload must reach a fresh LoadedMedia after LoadingMedia so resume hooks can fire");
}

void TestPlayerSetSource::testSetSourceThenClearInSameTurnDoesNotLoad()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.setSource(QUrl());

    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    QTest::qWait(3000);

    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
    for (const auto &args : statusSpy) {
        QVERIFY2(args.at(0).value<DragonPlayer::MediaStatus>() != DragonPlayer::MediaStatus::LoadedMedia, "a cleared source must never reach LoadedMedia");
    }
}

void TestPlayerSetSource::testSetSourceResetsPositionToZeroAndEmitsSignal()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    // Play track until EndOfMedia (position snaps to duration)
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTRY_VERIFY_WITH_TIMEOUT(helper.waitForEndOfMedia(15000), 15000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::EndOfMedia);
    QVERIFY2(player.position() > 0ms, "Position must be > 0 at EndOfMedia");

    // Setting a new source from EndOfMedia must reset position to 0 and emit positionChanged(0)
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QCOMPARE(player.position(), 0ms);
    QVERIFY2(posSpy.count() >= 1, "setSource() must emit positionChanged(0) when position was non-zero");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.position(), 0ms);

    // Seeking while stopped, then setting source again must also reset position to 0 and emit signal
    player.setPosition(1000ms);
    QCOMPARE(player.position(), 1000ms);

    posSpy.clear();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(player.position(), 0ms);
    QCOMPARE(posSpy.count(), 1);
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);

    // Setting source when position is already 0 must NOT emit positionChanged
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    posSpy.clear();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QCOMPARE(player.position(), 0ms);
    QCOMPARE(posSpy.count(), 0);
}

QTEST_MAIN(TestPlayerSetSource)
#include "test_player_setsource.moc"
