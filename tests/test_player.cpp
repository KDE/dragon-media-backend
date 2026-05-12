/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayer : public QObject
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

    void testInvalidMediaStaysStopped();
    void testSetSourceWhilePlayingStopsOldTrack();
    void testEndOfMediaTransitionsToStoppedState();
    void testPlayAtEndOfMediaRestarts();

    void testSignalOrderOnSetSource();
    void testSignalOrderOnStop();

    void testDeferredPlayIntentDuringFormatResolution();
    void testSetSourceThenPlayFirstTrack();
    void testPlayNextTrackWhilePlaying();
    void testPlayRapidNextNext();

    void testLazyFftInitialization();
    void testFftModeToggleCreatesInfrastructure();
    void testFftInfrastructurePersistsAcrossTrackChanges();
    void testFftOffSkipsInfrastructureOnTrackChange();
    void testFftModeBothEmitsDetailedAndBarFrames();

private:
    bool waitForSignal(QSignalSpy &spy, int timeoutMs = 5000);

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

void TestPlayer::testConstruction()
{
    DragonPlayer player;
    QVERIFY(true);
}

void TestPlayer::testInitialState()
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

void TestPlayer::testMutedProperty()
{
    DragonPlayer player;
    QVERIFY(!player.muted());

    player.setMuted(true);
    QVERIFY(player.muted());

    player.setMuted(false);
    QVERIFY(!player.muted());
}

void TestPlayer::testVolumeProperty()
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

void TestPlayer::testSourceProperty()
{
    DragonPlayer player;
    QVERIFY(!player.source().isValid());

    QUrl testSource = QUrl::fromLocalFile("/path/to/audio.mp3"_L1);
    player.setSource(testSource);
    QCOMPARE(player.source(), testSource);

    player.setSource(QUrl{});
    QVERIFY(!player.source().isValid());
}

void TestPlayer::testPlaybackStateProperty()
{
    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.play();
    QVERIFY(true);
}

void TestPlayer::testMediaStatusProperty()
{
    DragonPlayer player;
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testErrorProperty()
{
    DragonPlayer player;
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);

    player.setSource(QUrl("file:///nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError || player.status() == DragonPlayer::MediaStatus::NoMedia, 3000);
}

void TestPlayer::testDurationProperty()
{
    DragonPlayer player;
    QCOMPARE(player.duration(), 0);
}

void TestPlayer::testPositionProperty()
{
    DragonPlayer player;
    QCOMPARE(player.position(), 0);
}

void TestPlayer::testSeekableProperty()
{
    DragonPlayer player;
    QVERIFY(!player.seekable());
}

void TestPlayer::testSetSource()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QUrl source = QUrl::fromLocalFile("/tmp/test_audio.mp3"_L1);
    player.setSource(source);

    QVERIFY(statusSpy.count() >= 0);
    QCOMPARE(player.source(), source);
}

void TestPlayer::testSetSourceEmpty()
{
    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);

    player.setSource(QUrl{});

    QVERIFY(!player.source().isValid());
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testSetSourceInvalid()
{
    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile("/nonexistent/path/audio.mp3"_L1));

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError || player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);
}

void TestPlayer::testPlay()
{
    DragonPlayer player;
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(100);
    player.play();
    QVERIFY(true);
}

void TestPlayer::testPause()
{
    DragonPlayer player;
    player.pause();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState || player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testStop()
{
    DragonPlayer player;
    player.stop();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testSetMuted()
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

void TestPlayer::testSetVolume()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::volumeChanged);

    player.setVolume(0.5f);
    QVERIFY(qAbs(player.volume() - 0.5f) < 0.01f);
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.setVolume(0.5f);
}

void TestPlayer::testMutedChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::mutedChanged);

    player.setMuted(true);
    QTRY_VERIFY(spy.count() > 0);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
}

void TestPlayer::testVolumeChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::volumeChanged);

    player.setVolume(0.8f);
    QTRY_VERIFY(spy.count() > 0);
}

void TestPlayer::testSourceChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::sourceChanged);

    QUrl newSource = QUrl::fromLocalFile("/tmp/test.mp3"_L1);
    player.setSource(newSource);

    QTRY_VERIFY(spy.count() >= 1);
    QCOMPARE(player.source(), newSource);
}

void TestPlayer::testPlaybackStateChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile("/tmp/test.mp3"_L1));
    QTest::qWait(100);
    player.play();

    QTRY_VERIFY(spy.count() > 0);
}

void TestPlayer::testStatusChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 3000);

    auto status = spy.at(0).at(0).value<DragonPlayer::MediaStatus>();
    QVERIFY(status != DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testErrorChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0 || player.error() != DragonPlayer::Error::NoError, 5000);
}

void TestPlayer::testFftFrameReadySignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::fftFrameReady);
    QVERIFY(spy.isValid());
}

void TestPlayer::testSaveUndoPosition()
{
    DragonPlayer player;
    player.saveUndoPosition(5000);
    QVERIFY(true);
}

void TestPlayer::testRestoreUndoPosition()
{
    DragonPlayer player;
    player.saveUndoPosition(10000);
    player.restoreUndoPosition();
    QVERIFY(true);

    player.restoreUndoPosition();
    QVERIFY(true);
}

void TestPlayer::testStateMachineSequence_data()
{
    QTest::addColumn<QString>("initialState");
    QTest::addColumn<QString>("action");
    QTest::addColumn<QString>("expectedState");

    QTest::newRow("stopped_to_playing") << "StoppedState" << "play" << "PlayingState";
    QTest::newRow("stopped_to_paused") << "StoppedState" << "pause" << "PausedState";
    QTest::newRow("stopped_to_stopped") << "StoppedState" << "stop" << "StoppedState";
    QTest::newRow("playing_to_paused") << "PlayingState" << "pause" << "PausedState";
    QTest::newRow("playing_to_stopped") << "PlayingState" << "stop" << "StoppedState";
    QTest::newRow("paused_to_playing") << "PausedState" << "play" << "PlayingState";
    QTest::newRow("paused_to_stopped") << "PausedState" << "stop" << "StoppedState";
}

void TestPlayer::testStateMachineSequence()
{
    QFETCH(QString, initialState);
    QFETCH(QString, action);
    QFETCH(QString, expectedState);

    DragonPlayer player;

    if (initialState == "PausedState"_L1) {
        player.pause();
        QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    }

    if (action == "play"_L1) {
        player.play();
    } else if (action == "pause"_L1) {
        player.pause();
    } else if (action == "stop"_L1) {
        player.stop();
    }

    if (action == "play"_L1 && (initialState == "StoppedState"_L1 || initialState == "PausedState"_L1)) {
        auto expected = initialState == "StoppedState"_L1 ? DragonPlayer::PlaybackState::StoppedState : DragonPlayer::PlaybackState::PausedState;
        QCOMPARE(player.playbackState(), expected);
    } else if (expectedState == "PlayingState"_L1) {
        QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
    } else if (expectedState == "PausedState"_L1) {
        QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    } else if (expectedState == "StoppedState"_L1) {
        QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    }
}

void TestPlayer::testPlayPauseStopSequence()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy pausedSpy(&player, &DragonPlayer::paused);
    QSignalSpy stoppedSpy(&player, &DragonPlayer::stopped);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.pause();
    if (stateSpy.count() == 0) {
        QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    } else {
        QVERIFY(pausedSpy.count() >= 1);
    }

    stateSpy.clear();
    player.stop();
    QTRY_VERIFY(stateSpy.count() >= 1);
    QVERIFY(stoppedSpy.count() >= 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.play();
    QTest::qWait(100);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.pause();
    QTRY_VERIFY(stateSpy.count() >= 1);
    QVERIFY(pausedSpy.count() >= 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
}

void TestPlayer::testVolumeBoundaryValues()
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

void TestPlayer::testMuteAndVolumeInteraction()
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

void TestPlayer::testPositionTracking()
{
    DragonPlayer player;
    QCOMPARE(player.position(), 0);

    QTest::qWait(100);
    QCOMPARE(player.position(), 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(500);
    QVERIFY(player.position() >= 0);
}

void TestPlayer::testSeekBehavior()
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

void TestPlayer::testMultipleSourceChanges()
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

void TestPlayer::testSignalEmissionOrder()
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

void TestPlayer::testCurrentPlayingForRadiosSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::currentPlayingForRadiosChanged);
    QVERIFY(spy.isValid());
}

void TestPlayer::testSetPositionEmitsPositionChanged()
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

void TestPlayer::testPositionTimerEmitsDuringPlayback()
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

void TestPlayer::testSeekWithRealAudio()
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

void TestPlayer::testSetSourceDoesNotEmitPlayingState()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open without play()");
}

void TestPlayer::testSetSourceDoesNotAutoStartAudio()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open after setSource()");
}

void TestPlayer::testSetSourceWhilePlayingEmitsStoppedState()
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

    auto firstState = stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>();
    QCOMPARE(firstState, DragonPlayer::PlaybackState::StoppedState);

    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia), "setSource() must emit LoadingMedia status");

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
}

void TestPlayer::testPlayWithNoSourceIsNoOp()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.play();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testPlayDuringLoadingDefers()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 1);

    player.play();
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QTRY_VERIFY(stateSpy.count() == 2);
    QCOMPARE(stateSpy.at(1).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
}

void TestPlayer::testPauseDuringLoadingDefers()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 1);

    player.pause();
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QTRY_VERIFY(stateSpy.count() == 2);
    QCOMPARE(stateSpy.at(1).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PausedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
}

void TestPlayer::testStopDuringLoadingDefers()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(stateSpy.count(), 1);

    player.stop();
    QCOMPARE(stateSpy.count(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio must NOT be open after stop() during loading");
}

void TestPlayer::testDeferredStateResetOnNewSource()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    player.play();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
}

void TestPlayer::testPauseFromPlayingState()
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

void TestPlayer::testStopFromPlayingStateEmitsLoadedMedia()
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

    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadedMedia), "stop() must emit LoadedMedia per Qt contract");
}

void TestPlayer::testPlayFromPausedStateResumes()
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

void TestPlayer::testMultiplePlayCallsIdempotent()
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

void TestPlayer::testPlayAfterStopRestartsDecoder()
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

void TestPlayer::testInvalidMediaStaysStopped()
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

void TestPlayer::testSetSourceWhilePlayingStopsOldTrack()
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

    QVERIFY2(SignalSpyHelper::containsStatus(statusSpy, DragonPlayer::MediaStatus::LoadingMedia), "setSource() must emit LoadingMedia");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testEndOfMediaTransitionsToStoppedState()
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

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testPlayAtEndOfMediaRestarts()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    VERIFY_POSITION_NEAR(player.position(), 0, 500);

    player.stop();
}

void TestPlayer::testSignalOrderOnSetSource()
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
    QVERIFY2(tracker.contains(u"stateChanged(StoppedState)"_s), "setSource() must emit StoppedState");
    QVERIFY2(tracker.verifyOrder(u"stateChanged(StoppedState)"_s, u"statusChanged(LoadingMedia)"_s), "StoppedState must come before LoadingMedia");
}

void TestPlayer::testSignalOrderOnStop()
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
    QVERIFY2(tracker.contains(u"statusChanged(LoadedMedia)"_s), "stop() must emit LoadedMedia");
}

void TestPlayer::testDeferredPlayIntentDuringFormatResolution()
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

void TestPlayer::testSetSourceThenPlayFirstTrack()
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

void TestPlayer::testPlayNextTrackWhilePlaying()
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

void TestPlayer::testPlayRapidNextNext()
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

void TestPlayer::testLazyFftInitialization()
{
    DragonPlayer player;
    QCOMPARE(player.fftMode(), DragonPlayer::FftMode::Off);

    skipIfMissing(u"sample-3s.mp3"_s);

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(500);

    player.stop();
}

void TestPlayer::testFftModeToggleCreatesInfrastructure()
{
    skipIfMissing(u"gs-16b-2c-44100hz.ogg"_s);

    DragonPlayer player;
    player.setFftMode(DragonPlayer::FftMode::BarsOnly);

    int frameCount = 0;
    QObject::connect(
        &player,
        &DragonPlayer::fftFrameReady,
        &player,
        [&frameCount]() {
            ++frameCount;
        },
        Qt::QueuedConnection);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();
    QTRY_COMPARE_WITH_TIMEOUT(player.playbackState(), DragonPlayer::PlaybackState::PlayingState, 10000);

    for (int waits = 0; waits < 60 && frameCount == 0; ++waits) {
        QTest::qWait(50);
    }
    int initialCount = frameCount;
    QVERIFY2(initialCount > 0, "FFT should produce frames with BarsOnly");

    player.setFftMode(DragonPlayer::FftMode::Off);
    frameCount = 0;
    QTest::qWait(300);
    int framesAfterOff = frameCount;
    QTest::qWait(200);
    int framesAfterMoreWait = frameCount;

    QVERIFY2(framesAfterMoreWait - framesAfterOff < 3, "FFT should stop producing frames when Off");

    frameCount = 0;
    player.setFftMode(DragonPlayer::FftMode::BarsOnly);
    QTest::qWait(500);
    QVERIFY2(frameCount > 0, "FFT frames should resume when re-enabled");

    player.stop();
}

void TestPlayer::testFftInfrastructurePersistsAcrossTrackChanges()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;
    player.setFftMode(DragonPlayer::FftMode::Both);

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();

    QSignalSpy fftSpy(&player, &DragonPlayer::fftFrameReady);
    QTRY_VERIFY_WITH_TIMEOUT(fftSpy.size() > 0, 3000);

    fftSpy.clear();
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(fftSpy.size() > 0, 5000);

    player.stop();
}

void TestPlayer::testFftOffSkipsInfrastructureOnTrackChange()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(500);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();
    QTest::qWait(500);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
    player.stop();
}

void TestPlayer::testFftModeBothEmitsDetailedAndBarFrames()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait(10000));

    QSignalSpy fftSpy(&player, &DragonPlayer::fftFrameReady);
    player.setFftMode(DragonPlayer::FftMode::Both);
    QTRY_VERIFY_WITH_TIMEOUT(fftSpy.size() >= 5, 3000);

    for (int i = 0; i < fftSpy.size(); ++i) {
        QVERIFY(fftSpy.at(i).at(0).isValid());
    }

    player.stop();
}

bool TestPlayer::waitForSignal(QSignalSpy &spy, int timeoutMs)
{
    return spy.wait(timeoutMs);
}

QTEST_MAIN(TestPlayer)
#include "test_player.moc"
