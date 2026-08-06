/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Basic property, construction, and state-machine unit tests for DragonPlayer.
 * Some tests use audio fixtures for position/seek/undo verification.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMultimedia/dragonplayer.h>

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
    void testPlaybackStateNoSourceStaysStopped();
    void testErrorProperty();

    void testSetSource();
    void testSetSourceEmpty();
    void testSetSourceInvalid();
    void testPlayWithNoSourceIsNoOp();
    void testPause();
    void testStop();

    void testSetMuted();
    void testSetVolume();

    void testMutedChangedSignal();
    void testVolumeChangedSignal();
    void testSourceChangedSignal();
    void testNoPlayingStateOnInvalidSource();
    void testStatusChangedSignal();
    void testErrorChangedSignal();
    void testFftFrameReadySignal();

    void testSaveUndoPosition();
    void testRestoreUndoPosition();

    void testErrorString();
    void testErrorStringClearsOnSourceChange();
    void testErrorStringClearsOnValidSource();

    void testStateMachineSequence_data();
    void testStateMachineSequence();
    void testPlayPauseStopSequence();
    void testVolumeBoundaryValues();
    void testMuteAndVolumeInteraction();
    void testPositionTracking();
    void testSeekNoOpWithoutMedia();
    void testSeekBehaviorWithValidMedia();
    void testMultipleSourceChanges();
    void testSignalEmissionPresence();
    void testCurrentPlayingForRadiosSignal();
    void testSetPositionEmitsDefaultWithoutMedia();
    void testInvalidMediaStaysStopped();
};

void TestPlayerBasics::testConstruction()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testInitialState()
{
    DragonPlayer player;

    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);
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
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);

    player.setVolume(0.5);
    QVERIFY(qAbs(player.volume() - 0.5) < 0.01);

    player.setVolume(0.0);
    QVERIFY(qAbs(player.volume()) < 0.01);

    player.setVolume(1.0);
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);

    player.setVolume(2.0);
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);
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

void TestPlayerBasics::testPlaybackStateNoSourceStaysStopped()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testErrorProperty()
{
    DragonPlayer player;
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);

    player.setSource(QUrl("file:///nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 3000);
}

void TestPlayerBasics::testSetSource()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QUrl source = QUrl::fromLocalFile("/tmp/test_audio.mp3"_L1);
    player.setSource(source);

    QVERIFY(statusSpy.count() > 0);
    QCOMPARE(player.source(), source);
}

void TestPlayerBasics::testSetSourceEmpty()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);

    player.setSource(QUrl{});

    QVERIFY2(sourceSpy.count() >= 1, "setSource(empty) must emit sourceChanged");
    QVERIFY(!player.source().isValid());
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayerBasics::testSetSourceInvalid()
{
    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile("/nonexistent/path/audio.mp3"_L1));

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError && player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);
}

void TestPlayerBasics::testPlayWithNoSourceIsNoOp()
{
    DragonPlayer player;
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(100);
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testPause()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
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

    player.setVolume(0.5);
    QVERIFY(qAbs(player.volume() - 0.5) < 0.01);
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.setVolume(0.5);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testMutedChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::mutedChanged);

    player.setMuted(true);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);

    spy.clear();
    player.setMuted(false);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false);

    spy.clear();
    player.setMuted(false);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testVolumeChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::volumeChanged);

    player.setVolume(0.8);
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

void TestPlayerBasics::testNoPlayingStateOnInvalidSource()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::stateChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() != DragonPlayer::MediaStatus::LoadingMedia, 5000);
    player.stop();

    QVERIFY2(!SignalSpyHelper::containsState(spy, DragonPlayer::PlaybackState::PlayingState), "Loading an invalid source must never emit PlayingState");
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testStatusChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);

    auto status = spy.at(0).at(0).value<DragonPlayer::MediaStatus>();
    QVERIFY2(status == DragonPlayer::MediaStatus::LoadingMedia || status == DragonPlayer::MediaStatus::InvalidMedia,
             "First statusChanged should be LoadingMedia or InvalidMedia");
}

void TestPlayerBasics::testErrorChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 5000);
    QVERIFY(player.error() != DragonPlayer::Error::NoError);
}

void TestPlayerBasics::testFftFrameReadySignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::fftFrameReady);

    player.setFftMode(DragonPlayer::FftMode::BarsOnly);
    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() != DragonPlayer::MediaStatus::LoadingMedia, 5000);
    player.play();
    QTest::qWait(200);

    QVERIFY2(spy.count() == 0, "FFT frames must not be emitted when no audio is playing");
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer playingPlayer;
    PlayerHelper playingHelper(&playingPlayer);
    QSignalSpy playingSpy(&playingPlayer, &DragonPlayer::fftFrameReady);
    playingPlayer.setFftMode(DragonPlayer::FftMode::BarsOnly);
    QVERIFY(playingHelper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(playingHelper.playAndWait());
    QTRY_VERIFY_WITH_TIMEOUT(playingSpy.count() > 0, 5000);
    playingPlayer.stop();
}

void TestPlayerBasics::testSaveUndoPosition()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    const qint64 posBeforeSave = player.position();
    QVERIFY2(posBeforeSave > 0, "Position should have advanced during playback");

    player.saveUndoPosition(posBeforeSave);
    player.seek(0);
    QTRY_VERIFY_WITH_TIMEOUT(player.position() < posBeforeSave, 3000);

    player.restoreUndoPosition();
    QTRY_VERIFY_WITH_TIMEOUT(player.position() >= posBeforeSave - 200, 3000);

    player.stop();
}

void TestPlayerBasics::testRestoreUndoPosition()
{
    DragonPlayer player;

    player.saveUndoPosition(10000);
    player.restoreUndoPosition();
    QVERIFY2(player.position() == 0, "Position should remain 0 after restoreUndoPosition with no media undo position cannot be applied without loaded media");

    player.restoreUndoPosition();
    QVERIFY2(player.position() == 0, "Position should remain 0 after second restoreUndoPosition with no media undo stack should be empty");
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
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.stop();
    QCOMPARE(stateSpy.count(), 0);
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
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayerBasics::testVolumeBoundaryValues()
{
    DragonPlayer player;

    player.setVolume(0.0);
    QVERIFY(qAbs(player.volume()) < 0.01);

    player.setVolume(1.0);
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);

    player.setVolume(10.0);
    QVERIFY(qAbs(player.volume() - 1.0) < 0.01);

    player.setVolume(-1.0);
    QVERIFY(qAbs(player.volume()) < 0.01);
}

void TestPlayerBasics::testMuteAndVolumeInteraction()
{
    DragonPlayer player;

    player.setVolume(0.5);
    QVERIFY(qAbs(player.volume() - 0.5) < 0.01);

    player.setMuted(true);
    QVERIFY(player.muted());
    QVERIFY(qAbs(player.volume() - 0.5) < 0.01);

    player.setMuted(false);
    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume() - 0.5) < 0.01);

    player.setVolume(0.0);
    player.setMuted(true);
    QVERIFY(player.muted());
    QVERIFY(qAbs(player.volume()) < 0.01);

    player.setMuted(false);
    QVERIFY(!player.muted());
    QVERIFY(qAbs(player.volume()) < 0.01);
}

void TestPlayerBasics::testPositionTracking()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QCOMPARE(player.position(), 0);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    const qint64 posAfter500ms = player.position();
    QVERIFY2(posAfter500ms > 0, "Position must advance during playback");

    QTest::qWait(500);
    const qint64 posAfter1000ms = player.position();
    QVERIFY2(posAfter1000ms > posAfter500ms, "Position must continue advancing during playback");

    player.stop();
    const qint64 posAfterStop = player.position();
    QTest::qWait(300);
    QCOMPARE(player.position(), posAfterStop);
}

void TestPlayerBasics::testSeekNoOpWithoutMedia()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.seek(5000);
    QVERIFY2(posSpy.count() >= 1, "seek() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(10000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(-100);
    QCOMPARE(player.position(), 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(200);

    posSpy.clear();
    player.setPosition(2000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even with invalid source");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);

    posSpy.clear();
    player.seek(3000);
    QVERIFY2(posSpy.count() >= 1, "seek() must emit positionChanged even with invalid source");
    QCOMPARE(player.position(), 0);
}

void TestPlayerBasics::testSeekBehaviorWithValidMedia()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(player.duration() > 0);
    QVERIFY(player.seekable());

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.play();
    QVERIFY2(helper.waitForState(DragonPlayer::PlaybackState::PlayingState, 10000), "Failed to reach PlayingState");

    QTest::qWait(200);
    posSpy.clear();

    player.seek(1000);
    QTRY_VERIFY_WITH_TIMEOUT(posSpy.count() >= 1, 5000);
    QVERIFY2(player.position() >= 800 && player.position() <= 1200, qPrintable(u"Position should be near 1000ms after seek, got %1"_s.arg(player.position())));

    player.stop();
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

void TestPlayerBasics::testSignalEmissionPresence()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy seekableSpy(&player, &DragonPlayer::seekableChanged);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    QVERIFY(player.source().isValid());
    QVERIFY2(seekableSpy.count() >= 1, "seekableChanged must be emitted after loading valid media");
    QVERIFY(player.seekable());
}

void TestPlayerBasics::testCurrentPlayingForRadiosSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::currentPlayingForRadiosChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() != DragonPlayer::MediaStatus::LoadingMedia, 5000);

    QVERIFY2(spy.count() == 0, "currentPlayingForRadiosChanged must not fire for local file sources");
}

void TestPlayerBasics::testSetPositionEmitsDefaultWithoutMedia()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(5000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(10000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);
}

void TestPlayerBasics::testInvalidMediaStaysStopped()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);

    QVERIFY2(!SignalSpyHelper::containsState(stateSpy, DragonPlayer::PlaybackState::PlayingState), "Invalid media must never emit PlayingState");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(errorSpy.count() >= 1);
}

void TestPlayerBasics::testErrorString()
{
    DragonPlayer player;
    QVERIFY(player.errorString().isEmpty());
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::InvalidMedia);
    QVERIFY2(!player.errorString().isEmpty(), "errorString() must describe the error once error() is set");

    // Like QMediaPlayer, the error and its description persist across stop()
    // until a new source is set.
    player.stop();
    QCOMPARE(player.error(), DragonPlayer::Error::FormatError);
    QVERIFY(!player.errorString().isEmpty());
}

void TestPlayerBasics::testErrorStringClearsOnSourceChange()
{
    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);
    QVERIFY(!player.errorString().isEmpty());

    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl{});

    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);
    QVERIFY2(player.errorString().isEmpty(), "errorString() must clear when the source is reset");
    QVERIFY(errorSpy.count() >= 1);
    QCOMPARE(errorSpy.last().at(0).value<DragonPlayer::Error>(), DragonPlayer::Error::NoError);
}

void TestPlayerBasics::testErrorStringClearsOnValidSource()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);
    QVERIFY(!player.errorString().isEmpty());

    PlayerHelper helper(&player);
    QVERIFY2(helper.setSourceAndWait(u"sample-3s.mp3"_s), "Valid source must load after a failed one");

    QCOMPARE(player.error(), DragonPlayer::Error::NoError);
    QVERIFY2(player.errorString().isEmpty(), "errorString() must clear once a new source loads successfully");
}

QTEST_MAIN(TestPlayerBasics)
#include "test_player_basics.moc"
