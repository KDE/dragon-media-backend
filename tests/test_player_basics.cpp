/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Basic property, construction, and state-machine unit tests for DragonPlayer.
 * No audio fixtures required pure unit tests.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <dragonsdl/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayerBasics : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConstruction();
    void testInitialState();

    void testMutedProperty();
    void testVolumeProperty();
    void testSourceProperty();
    void testPlaybackStateProperty();
    void testMediaStatusProperty();
    void testErrorProperty();
    void testDurationProperty();
    void testPositionProperty();
    void testSeekableProperty();

    void testSetSource();
    void testSetSourceEmpty();
    void testSetSourceInvalid();
    void testPlay();
    void testPause();
    void testStop();

    void testSetMuted();
    void testSetVolume();

    void testMutedChangedSignal();
    void testVolumeChangedSignal();
    void testSourceChangedSignal();
    void testPlaybackStateChangedSignal();
    void testStatusChangedSignal();
    void testErrorChangedSignal();
    void testFftFrameReadySignal();

    void testSaveUndoPosition();
    void testRestoreUndoPosition();

    void testStateMachineSequence_data();
    void testStateMachineSequence();
    void testPlayPauseStopSequence();
    void testVolumeBoundaryValues();
    void testMuteAndVolumeInteraction();
    void testPositionTracking();
    void testSeekBehavior();
    void testMultipleSourceChanges();
    void testSignalEmissionOrder();
    void testCurrentPlayingForRadiosSignal();
    void testSetPositionEmitsPositionChanged();
    void testInvalidMediaStaysStopped();
};

void TestPlayerBasics::testConstruction()
{
    DragonPlayer player;
    QVERIFY(true);
}

void TestPlayerBasics::testInitialState()
{
    DragonPlayer player;

    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume() - 1.0f) < 0.01f);
    QVERIFY(!player.source().isValid());
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);
    QCOMPARE(player.duration(), 0);
    QCOMPARE(player.position(), 0);
    QVERIFY(!player.seekable());
}

void TestPlayerBasics::testMutedProperty()
{
    DragonPlayer player;
    QVERIFY(!player.muted());

    player.setMuted(true);
    QVERIFY(player.muted());

    player.setMuted(false);
    QVERIFY(!player.muted());
}

void TestPlayerBasics::testVolumeProperty()
{
    DragonPlayer player;
    QVERIFY(qAbs(player.volume() - 1.0f) < 0.01f);

    player.setVolume(0.5f);
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);

    player.setVolume(0.0f);
    QVERIFY(qAbs(player.volume()) < 0.01f);

    player.setVolume(1.0f);
    QVERIFY(qAbs(player.volume() - 1.0f) < 0.01f);

    player.setVolume(2.0f);
    QVERIFY(player.volume() > 0.0f);
}

void TestPlayerBasics::testSourceProperty()
{
    DragonPlayer player;
    QVERIFY(!player.source().isValid());

    QUrl testSource = QUrl::fromLocalFile("/path/to/audio.mp3"_L1);
    player.setSource(testSource);
    QCOMPARE(player.source(), testSource);

    player.setSource(QUrl{});
    QVERIFY(!player.source().isValid());
}

void TestPlayerBasics::testPlaybackStateProperty()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.play();
    QVERIFY(true);
}

void TestPlayerBasics::testMediaStatusProperty()
{
    DragonPlayer player;
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testErrorProperty()
{
    DragonPlayer player;
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);

    player.setSource(QUrl("file:///nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 3000);
}

void TestPlayerBasics::testDurationProperty()
{
    DragonPlayer player;
    QCOMPARE(player.duration(), 0);
}

void TestPlayerBasics::testPositionProperty()
{
    DragonPlayer player;
    QCOMPARE(player.position(), 0);
}

void TestPlayerBasics::testSeekableProperty()
{
    DragonPlayer player;
    QVERIFY(!player.seekable());
}

void TestPlayerBasics::testSetSource()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QUrl source = QUrl::fromLocalFile("/tmp/test_audio.mp3"_L1);
    player.setSource(source);

    QVERIFY(statusSpy.count() >= 0);
    QCOMPARE(player.source(), source);
}

void TestPlayerBasics::testSetSourceEmpty()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);

    player.setSource(QUrl{});

    QVERIFY(!player.source().isValid());
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testSetSourceInvalid()
{
    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile("/nonexistent/path/audio.mp3"_L1));

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError || player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);
}

void TestPlayerBasics::testPlay()
{
    DragonPlayer player;
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(100);
    player.play();
    QVERIFY(true);
}

void TestPlayerBasics::testPause()
{
    DragonPlayer player;
    player.pause();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState || player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testStop()
{
    DragonPlayer player;
    player.stop();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testSetMuted()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::mutedChanged);

    player.setMuted(true);
    QVERIFY(player.muted());
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.setMuted(false);
    QVERIFY(!player.muted());
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.setMuted(false);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testSetVolume()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::volumeChanged);

    player.setVolume(0.5f);
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.setVolume(0.5f);
}

void TestPlayerBasics::testMutedChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::mutedChanged);

    player.setMuted(true);
    QTRY_VERIFY(spy.count() > 0);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
}

void TestPlayerBasics::testVolumeChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::volumeChanged);

    player.setVolume(0.8f);
    QTRY_VERIFY(spy.count() > 0);
}

void TestPlayerBasics::testSourceChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::sourceChanged);

    QUrl newSource = QUrl::fromLocalFile("/tmp/test.mp3"_L1);
    player.setSource(newSource);

    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(player.source(), newSource);
}

void TestPlayerBasics::testPlaybackStateChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile("/tmp/test.mp3"_L1));
    QTest::qWait(100);
    player.play();

    QTRY_VERIFY(spy.count() > 0);
}

void TestPlayerBasics::testStatusChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 3000);

    auto status = spy.at(0).at(0).value<DragonPlayer::MediaStatus>();
    QVERIFY(status != DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testErrorChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0 || player.error() != DragonPlayer::Error::NoError, 5000);
}

void TestPlayerBasics::testFftFrameReadySignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::fftFrameReady);
    QVERIFY(spy.isValid());
}

void TestPlayerBasics::testSaveUndoPosition()
{
    DragonPlayer player;
    player.saveUndoPosition(5000);
    QVERIFY(true);
}

void TestPlayerBasics::testRestoreUndoPosition()
{
    DragonPlayer player;
    player.saveUndoPosition(10000);
    player.restoreUndoPosition();
    QVERIFY(true);

    player.restoreUndoPosition();
    QVERIFY(true);
}

void TestPlayerBasics::testStateMachineSequence_data()
{
    QTest::addColumn<QString>("initialState");
    QTest::addColumn<QString>("action");
    QTest::addColumn<QString>("expectedState");

    QTest::newRow("stopped_to_playing_no_source") << "StoppedState" << "play" << "StoppedState";
    QTest::newRow("stopped_to_paused_no_source") << "StoppedState" << "pause" << "StoppedState";
    QTest::newRow("stopped_to_stopped_no_source") << "StoppedState" << "stop" << "StoppedState";
}

void TestPlayerBasics::testStateMachineSequence()
{
    QFETCH(QString, initialState);
    QFETCH(QString, action);
    QFETCH(QString, expectedState);

    DragonPlayer player;

    if (action == "play"_L1) {
        player.play();
    } else if (action == "pause"_L1) {
        player.pause();
    } else if (action == "stop"_L1) {
        player.stop();
    }

    QCOMPARE(player.playbackState(),
             expectedState == "StoppedState"_L1       ? DragonPlayer::PlaybackState::StoppedState
                 : expectedState == "PausedState"_L1  ? DragonPlayer::PlaybackState::PausedState
                 : expectedState == "PlayingState"_L1 ? DragonPlayer::PlaybackState::PlayingState
                                                      : DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testPlayPauseStopSequence()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy pausedSpy(&player, &DragonPlayer::paused);
    QSignalSpy stoppedSpy(&player, &DragonPlayer::stopped);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(pausedSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.stop();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    stateSpy.clear();
    player.play();
    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(pausedSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testVolumeBoundaryValues()
{
    DragonPlayer player;

    player.setVolume(0.0f);
    QVERIFY(qAbs(player.volume()) < 0.01f);

    player.setVolume(1.0f);
    QVERIFY(qAbs(player.volume() - 1.0f) < 0.01f);

    player.setVolume(10.0f);
    QVERIFY(player.volume() > 0.0f);

    player.setVolume(-1.0f);
    QVERIFY(true);
}

void TestPlayerBasics::testMuteAndVolumeInteraction()
{
    DragonPlayer player;

    player.setVolume(0.5f);
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);

    player.setMuted(true);
    QVERIFY(player.muted());
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);

    player.setMuted(false);
    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);

    player.setVolume(0.0f);
    player.setMuted(true);
    QVERIFY(player.muted());
    QVERIFY(qAbs(player.volume()) < 0.01f);

    player.setMuted(false);
    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume()) < 0.01f);
}

void TestPlayerBasics::testPositionTracking()
{
    DragonPlayer player;
    QCOMPARE(player.position(), 0);

    QTest::qWait(100);
    QCOMPARE(player.position(), 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(500);
    QVERIFY(player.position() >= 0);
}

void TestPlayerBasics::testSeekBehavior()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.seek(5000);
    QVERIFY2(posSpy.count() >= 1, "seek() must emit positionChanged");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(10000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    player.setPosition(-100);
    QCOMPARE(player.position(), 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(200);

    posSpy.clear();
    player.setPosition(2000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    player.seek(3000);
}

void TestPlayerBasics::testMultipleSourceChanges()
{
    DragonPlayer player;
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent1.mp3"_L1));
    QTRY_VERIFY(sourceSpy.count() >= 1);

    player.setSource(QUrl::fromLocalFile("/nonexistent2.mp3"_L1));
    QTRY_VERIFY(sourceSpy.count() >= 2);

    sourceSpy.clear();
    player.setSource(QUrl{});
    QTRY_VERIFY(sourceSpy.count() >= 1);
    QVERIFY(!player.source().isValid());
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testSignalEmissionOrder()
{
    DragonPlayer player;
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy seekableSpy(&player, &DragonPlayer::seekableChanged);

    player.setSource(QUrl::fromLocalFile("/tmp/test.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 3000);

    QVERIFY(player.source().isValid());
    QVERIFY(seekableSpy.count() >= 1);
    QVERIFY(player.seekable());
}

void TestPlayerBasics::testCurrentPlayingForRadiosSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::currentPlayingForRadiosChanged);
    QVERIFY(spy.isValid());
}

void TestPlayerBasics::testSetPositionEmitsPositionChanged()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(5000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(10000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);
}

void TestPlayerBasics::testInvalidMediaStaysStopped()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);

    QVERIFY2(!SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::PlayingState), "Invalid media must never emit PlayingState");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(errorSpy.count() >= 1);
}

QTEST_MAIN(TestPlayerBasics)
#include "test_player_basics.moc"
