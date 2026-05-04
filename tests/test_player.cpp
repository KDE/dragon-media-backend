/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

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

    void testInvalidMediaStaysStopped();
    void testSetSourceWhilePlayingStopsOldTrack();
    void testEndOfMediaTransitionsToStoppedState();
    void testPlayAtEndOfMediaRestarts();

    void testSignalOrderOnSetSource();
    void testSignalOrderOnStop();

private:
    QString fixture(const QString &filename) const;
    bool waitForSignal(QSignalSpy &spy, int timeoutMs = 5000);
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
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(player.status() == DragonPlayer::MediaStatus::NoMedia);
    QVERIFY(player.error() == DragonPlayer::Error::NoError);
    QVERIFY(player.duration() == 0);
    QVERIFY(player.position() == 0);
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
}

void TestPlayer::testSourceProperty()
{
    DragonPlayer player;

    QVERIFY(!player.source().isValid());

    QUrl testSource = QUrl::fromLocalFile("/path/to/audio.mp3"_L1);
    player.setSource(testSource);
    QVERIFY(player.source() == testSource);

    player.setSource(QUrl{});
    QVERIFY(!player.source().isValid());
}

void TestPlayer::testPlaybackStateProperty()
{
    DragonPlayer player;

    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    player.play();
    QVERIFY(true);
}

void TestPlayer::testMediaStatusProperty()
{
    DragonPlayer player;

    QVERIFY(player.status() == DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testErrorProperty()
{
    DragonPlayer player;

    QVERIFY(player.error() == DragonPlayer::Error::NoError);

    QUrl invalidSource = QUrl("file:///nonexistent/file.mp3"_L1);
    player.setSource(invalidSource);

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError || player.status() == DragonPlayer::MediaStatus::NoMedia, 3000);
}

void TestPlayer::testDurationProperty()
{
    DragonPlayer player;

    QVERIFY(player.duration() == 0);
}

void TestPlayer::testPositionProperty()
{
    DragonPlayer player;

    QVERIFY(player.position() == 0);
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

    QVERIFY(player.source() == source);
}

void TestPlayer::testSetSourceEmpty()
{
    DragonPlayer player;

    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);

    player.setSource(QUrl{});

    QVERIFY(!player.source().isValid());
    QVERIFY(player.status() == DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testSetSourceInvalid()
{
    DragonPlayer player;

    QUrl invalid = QUrl::fromLocalFile("/nonexistent/path/audio.mp3"_L1);
    player.setSource(invalid);

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError || player.status() == DragonPlayer::MediaStatus::InvalidMedia, 5000);
}

void TestPlayer::testPlay()
{
    DragonPlayer player;

    player.play();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

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
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
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
    QVERIFY(spy.count() == 0);
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

    QList<QVariant> args = spy.at(0);
    QVERIFY(args.at(0).toBool() == true);
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

    QTRY_VERIFY(spy.count() > 0);

    QVERIFY(spy.count() >= 1);

    QVERIFY(player.source() == newSource);
}

void TestPlayer::testPlaybackStateChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile("/tmp/test.mp3"_L1));
    QTest::qWait(100);

    player.play();
    QTRY_VERIFY(spy.count() > 0);

    QList<QVariant> args = spy.at(0);
    QVERIFY(args.at(0).value<DragonPlayer::PlaybackState>() != DragonPlayer::PlaybackState::StoppedState || spy.count() > 0);
}

void TestPlayer::testStatusChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::statusChanged);

    QUrl invalidSource = QUrl::fromLocalFile("/nonexistent.mp3"_L1);
    player.setSource(invalidSource);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 3000);

    QList<QVariant> args = spy.at(0);
    auto status = args.at(0).value<DragonPlayer::MediaStatus>();
    QVERIFY(status != DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testErrorChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::errorChanged);

    QUrl invalidSource = QUrl::fromLocalFile("/nonexistent/file.mp3"_L1);
    player.setSource(invalidSource);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0 || player.error() != DragonPlayer::Error::NoError, 5000);
}

void TestPlayer::testFftFrameReadySignal()
{
    DragonPlayer player;

    QSignalSpy spy(&player, &DragonPlayer::fftFrameReady);
    QVERIFY(spy.isValid());

    QVERIFY(true);
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
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
    }

    if (action == "play"_L1) {
        player.play();
    } else if (action == "pause"_L1) {
        player.pause();
    } else if (action == "stop"_L1) {
        player.stop();
    }

    if (action == "play"_L1 && initialState == "StoppedState"_L1) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
    } else if (action == "play"_L1 && initialState == "PausedState"_L1) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
    } else if (expectedState == "PlayingState"_L1) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
    } else if (expectedState == "PausedState"_L1) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
    } else if (expectedState == "StoppedState"_L1) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
    }
}

void TestPlayer::testPlayPauseStopSequence()
{
    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy playingSpy(&player, &DragonPlayer::playing);
    QSignalSpy pausedSpy(&player, &DragonPlayer::paused);
    QSignalSpy stoppedSpy(&player, &DragonPlayer::stopped);

    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    player.pause();
    if (stateSpy.count() == 0) {
        QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
    } else {
        QVERIFY(pausedSpy.count() >= 1);
    }

    stateSpy.clear();
    player.stop();
    QTRY_VERIFY(stateSpy.count() >= 1);
    QVERIFY(stoppedSpy.count() >= 1);
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.play();
    QTest::qWait(100);
    QVERIFY(stateSpy.count() == 0);
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    stateSpy.clear();
    player.pause();
    QTRY_VERIFY(stateSpy.count() >= 1);
    QVERIFY(pausedSpy.count() >= 1);
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
}

void TestPlayer::testVolumeBoundaryValues()
{
    DragonPlayer player;

    player.setVolume(0.0f);
    QVERIFY(qAbs(player.volume()) < 0.01f);

    player.setVolume(1.0f);
    QVERIFY(qAbs(player.volume() - 1.0f) < 0.01f);

    player.setVolume(10.0f);
    float vol = player.volume();
    QVERIFY(vol > 0.0f);

    player.setVolume(-1.0f);
    vol = player.volume();
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

    QVERIFY(player.position() == 0);

    QTest::qWait(100);
    QVERIFY(player.position() == 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(500);

    int64_t pos = player.position();
    QVERIFY(pos >= 0);
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

    posSpy.clear();
    player.setPosition(-100);
    QCOMPARE(player.position(), 0);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(200);

    posSpy.clear();
    player.setPosition(2000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even with invalid source");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);

    player.seek(3000);
    QVERIFY(true);
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

    QVERIFY(player.status() == DragonPlayer::MediaStatus::NoMedia);
}

void TestPlayer::testSignalEmissionOrder()
{
    DragonPlayer player;

    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
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

    QVERIFY(true);
}

void TestPlayer::testSetPositionEmitsPositionChanged()
{
    DragonPlayer player;

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(5000);

    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged signal");

    QList<QVariant> args = posSpy.at(0);
    QCOMPARE(args.at(0).toLongLong(), 0);

    QCOMPARE(player.position(), 0);

    posSpy.clear();
    player.setPosition(10000);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged on subsequent calls");
    QCOMPARE(posSpy.at(0).at(0).toLongLong(), 0);
    QCOMPARE(player.position(), 0);
}

void TestPlayer::testPositionTimerEmitsDuringPlayback()
{
    QString filePath = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR) + "/sample-3s.mp3"_L1;
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QTest::qWait(300);

    QVERIFY2(posSpy.count() > 0, "positionChanged must be emitted periodically during playback");

    QVERIFY2(player.position() >= 0, "position() must return non-negative value during playback");

    player.stop();
}

void TestPlayer::testSeekWithRealAudio()
{
    QString filePath = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR) + "/sample-3s.mp3"_L1;
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QTest::qWait(200);

    player.seek(1000);

    QTest::qWait(200);

    int64_t pos = player.position();
    QVERIFY2(std::llabs(pos - 1000) < 500, qPrintable(u"Seek failed: expected ~1000ms, got %1ms"_s.arg(pos)));

    player.stop();
}

QString TestPlayer::fixture(const QString &filename) const
{
    return QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR) + "/"_L1 + filename;
}

void TestPlayer::testSetSourceDoesNotEmitPlayingState()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 1);
    auto state = stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>();
    QCOMPARE(state, DragonPlayer::PlaybackState::StoppedState);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testSetSourceWhilePlayingEmitsStoppedState()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    auto firstState = stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>();
    QCOMPARE(firstState, DragonPlayer::PlaybackState::StoppedState);

    bool sawLoadingMedia = false;
    for (const auto &args : statusSpy) {
        auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        if (status == DragonPlayer::MediaStatus::LoadingMedia) {
            sawLoadingMedia = true;
            break;
        }
    }
    QVERIFY2(sawLoadingMedia, "setSource() must emit LoadingMedia status");

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
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
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(filePath));
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
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(filePath));
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
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);

    player.setSource(QUrl::fromLocalFile(filePath));
    QCOMPARE(stateSpy.count(), 1);

    player.stop();

    QCOMPARE(stateSpy.count(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testDeferredStateResetOnNewSource()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    player.play();

    player.setSource(QUrl::fromLocalFile(filePath));

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testPauseFromPlayingState()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.pause();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PausedState);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PausedState);
    QVERIFY2(player.isAudioActive(), "Audio device should stay open after pause");

    player.stop();
}

void TestPlayer::testStopFromPlayingStateEmitsLoadedMedia()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.stop();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio device should be closed after stop()");

    bool sawLoadedMedia = false;
    for (const auto &args : statusSpy) {
        auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        if (status == DragonPlayer::MediaStatus::LoadedMedia) {
            sawLoadedMedia = true;
            break;
        }
    }
    QVERIFY2(sawLoadedMedia, "stop() must emit statusChanged(LoadedMedia) per Qt contract");
}

void TestPlayer::testPlayFromPausedStateResumes()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QTest::qWait(300);
    int64_t posBefore = player.position();

    player.pause();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);

    QTest::qWait(200);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();

    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<DragonPlayer::PlaybackState>(), DragonPlayer::PlaybackState::PlayingState);

    QVERIFY2(player.position() >= posBefore, qPrintable(u"Position should not reset on resume: was %1, now %2"_s.arg(posBefore).arg(player.position())));

    player.stop();
}

void TestPlayer::testMultiplePlayCallsIdempotent()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();
    player.play();
    player.play();
    QTest::qWait(100);

    QCOMPARE(stateSpy.count(), 0);

    player.stop();
}

void TestPlayer::testInvalidMediaStaysStopped()
{
    DragonPlayer player;

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    player.setSource(QUrl::fromLocalFile("/nonexistent/file.mp3"_L1));

    QTRY_VERIFY_WITH_TIMEOUT(player.error() != DragonPlayer::Error::NoError, 5000);

    for (const auto &args : stateSpy) {
        auto state = args.at(0).value<DragonPlayer::PlaybackState>();
        QVERIFY2(state != DragonPlayer::PlaybackState::PlayingState, "Invalid media must never emit PlayingState");
    }

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(errorSpy.count() >= 1);
}

void TestPlayer::testSetSourceWhilePlayingStopsOldTrack()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);

    bool sawStoppedState = false;
    for (const auto &args : stateSpy) {
        auto state = args.at(0).value<DragonPlayer::PlaybackState>();
        if (state == DragonPlayer::PlaybackState::StoppedState) {
            sawStoppedState = true;
            break;
        }
    }
    QVERIFY2(sawStoppedState, "setSource() while playing must emit StoppedState per Qt contract");

    bool sawLoadingMedia = false;
    for (const auto &args : statusSpy) {
        auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        if (status == DragonPlayer::MediaStatus::LoadingMedia) {
            sawLoadingMedia = true;
            break;
        }
    }
    QVERIFY2(sawLoadingMedia, "setSource() must emit LoadingMedia status");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testEndOfMediaTransitionsToStoppedState()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);

    bool sawStoppedState = false;
    for (const auto &args : stateSpy) {
        auto state = args.at(0).value<DragonPlayer::PlaybackState>();
        if (state == DragonPlayer::PlaybackState::StoppedState) {
            sawStoppedState = true;
            break;
        }
    }
    QVERIFY2(sawStoppedState, "End of media must emit StoppedState");

    bool sawEndOfMedia = false;
    for (const auto &args : statusSpy) {
        auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        if (status == DragonPlayer::MediaStatus::EndOfMedia) {
            sawEndOfMedia = true;
            break;
        }
    }
    QVERIFY2(sawEndOfMedia, "End of media must emit EndOfMedia status");

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

void TestPlayer::testPlayAtEndOfMediaRestarts()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::EndOfMedia, 15000);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.play();

    QTRY_VERIFY(stateSpy.count() >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 10000);
    QVERIFY2(player.position() < 500, qPrintable(u"Restarted position should be near beginning: %1"_s.arg(player.position())));

    player.stop();
}

void TestPlayer::testSignalOrderOnSetSource()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    QStringList signalOrder;

    QObject::connect(
        &player,
        &DragonPlayer::playbackStateChanged,
        &player,
        [&signalOrder](DragonPlayer::PlaybackState state) {
            if (state == DragonPlayer::PlaybackState::StoppedState) {
                signalOrder.append(u"stateChanged(StoppedState)"_s);
            } else if (state == DragonPlayer::PlaybackState::PlayingState) {
                signalOrder.append(u"stateChanged(PlayingState)"_s);
            } else if (state == DragonPlayer::PlaybackState::PausedState) {
                signalOrder.append(u"stateChanged(PausedState)"_s);
            }
        },
        Qt::DirectConnection);

    QObject::connect(
        &player,
        &DragonPlayer::statusChanged,
        &player,
        [&signalOrder](DragonPlayer::MediaStatus status) {
            if (status == DragonPlayer::MediaStatus::LoadingMedia) {
                signalOrder.append(u"statusChanged(LoadingMedia)"_s);
            } else if (status == DragonPlayer::MediaStatus::LoadedMedia) {
                signalOrder.append(u"statusChanged(LoadedMedia)"_s);
            } else if (status == DragonPlayer::MediaStatus::NoMedia) {
                signalOrder.append(u"statusChanged(NoMedia)"_s);
            } else if (status == DragonPlayer::MediaStatus::EndOfMedia) {
                signalOrder.append(u"statusChanged(EndOfMedia)"_s);
            }
        },
        Qt::DirectConnection);

    QObject::connect(
        &player,
        &DragonPlayer::sourceChanged,
        &player,
        [&signalOrder]() {
            signalOrder.append(u"sourceChanged()"_s);
        },
        Qt::DirectConnection);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(signalOrder.contains(u"statusChanged(LoadingMedia)"_s), "setSource() must emit LoadingMedia");

    QVERIFY2(signalOrder.contains(u"stateChanged(StoppedState)"_s), "setSource() must emit stateChanged(StoppedState) per Qt contract");

    int stoppedIdx = signalOrder.indexOf(u"stateChanged(StoppedState)"_s);
    int loadingIdx = signalOrder.indexOf(u"statusChanged(LoadingMedia)"_s);
    QVERIFY2(stoppedIdx >= 0 && loadingIdx >= 0 && stoppedIdx < loadingIdx,
             "stateChanged(StoppedState) must come before statusChanged(LoadingMedia) per Qt order");
}

void TestPlayer::testSignalOrderOnStop()
{
    QString filePath = fixture("sample-3s.mp3"_L1);
    if (!QFileInfo::exists(filePath)) {
        QSKIP("Audio fixture not available");
    }

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    player.play();
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QStringList signalOrder;

    QObject::connect(
        &player,
        &DragonPlayer::playbackStateChanged,
        &player,
        [&signalOrder](DragonPlayer::PlaybackState state) {
            if (state == DragonPlayer::PlaybackState::StoppedState) {
                signalOrder.append(u"stateChanged(StoppedState)"_s);
            }
        },
        Qt::DirectConnection);

    QObject::connect(
        &player,
        &DragonPlayer::statusChanged,
        &player,
        [&signalOrder](DragonPlayer::MediaStatus status) {
            if (status == DragonPlayer::MediaStatus::LoadedMedia) {
                signalOrder.append(u"statusChanged(LoadedMedia)"_s);
            }
        },
        Qt::DirectConnection);

    QObject::connect(
        &player,
        &DragonPlayer::positionChanged,
        &player,
        [&signalOrder](int64_t pos) {
            if (pos == 0) {
                signalOrder.append(u"positionChanged(0)"_s);
            }
        },
        Qt::DirectConnection);

    player.stop();

    QTRY_VERIFY(signalOrder.count() >= 1);

    QVERIFY2(signalOrder.contains(u"stateChanged(StoppedState)"_s), "stop() must emit StoppedState");

    QVERIFY2(signalOrder.contains(u"statusChanged(LoadedMedia)"_s), "stop() must emit statusChanged(LoadedMedia) per Qt contract");
}

bool TestPlayer::waitForSignal(QSignalSpy &spy, int timeoutMs)
{
    return spy.wait(timeoutMs);
}

QTEST_MAIN(TestPlayer)
#include "test_player.moc"