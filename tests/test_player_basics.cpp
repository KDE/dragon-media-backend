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

#include "sink/dragonaudiosink.h"
#include <DragonPlayer>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

class TestPlayerBasics : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFormatChangeRequiredRespectsFlushPending();
    void testFormatChangeRequiredUngatesQueueWhenNoFlushPending();
    void testConstruction();
    void testInitialState();

    void testMutedProperty();
    void testVolumeProperty();
    void testStreamNameProperty();
    void testPropertiesAreFinal();
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

    void testErrorString();
    void testErrorStringClearsOnSourceChange();
    void testErrorStringClearsOnValidSource();

    void testStateMachineSequence_data();
    void testStateMachineSequence();
    void testPlayPauseStopSequence();
    void testPauseWhileStoppedWithLoadedMedia();
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

    QVERIFY(!player.audioOutput()->isMuted());
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);
    QVERIFY(!player.source().isValid());
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);
    QCOMPARE(player.error(), DragonPlayer::Error::NoError);
    QCOMPARE(player.duration(), std::nullopt);
    QCOMPARE(player.position(), 0ms);
    QVERIFY(!player.isSeekable());
    QCOMPARE(player.streamName(), QString());
}

void TestPlayerBasics::testStreamNameProperty()
{
    DragonPlayer player;
    QCOMPARE(player.streamName(), QString());

    QSignalSpy spy(&player, &DragonPlayer::streamNameChanged);

    player.setStreamName(u"My Stream"_s);
    QCOMPARE(player.streamName(), u"My Stream"_s);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), u"My Stream"_s);

    // Setting same name should not re-emit
    player.setStreamName(u"My Stream"_s);
    QCOMPARE(spy.count(), 1);

    // QMetaProperty read/write
    const int propIndex = player.metaObject()->indexOfProperty("streamName");
    QVERIFY(propIndex != -1);
    const QMetaProperty prop = player.metaObject()->property(propIndex);
    QVERIFY(prop.isFinal());
    QCOMPARE(prop.read(&player).toString(), u"My Stream"_s);

    QVERIFY(prop.write(&player, u"Another Name"_s));
    QCOMPARE(player.streamName(), u"Another Name"_s);
    QCOMPARE(spy.count(), 2);
}

void TestPlayerBasics::testPropertiesAreFinal()
{
    DragonPlayer player;
    const auto *mo = player.metaObject();
    for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
        const auto prop = mo->property(i);
        QVERIFY2(prop.isFinal(), qPrintable(u"DragonPlayer property '%1' must be FINAL"_s.arg(QString::fromUtf8(prop.name()))));
    }

    const auto *outputMo = player.audioOutput()->metaObject();
    for (int i = outputMo->propertyOffset(); i < outputMo->propertyCount(); ++i) {
        const auto prop = outputMo->property(i);
        QVERIFY2(prop.isFinal(), qPrintable(u"DragonAudioOutput property '%1' must be FINAL"_s.arg(QString::fromUtf8(prop.name()))));
    }
}

void TestPlayerBasics::testMutedProperty()
{
    DragonPlayer player;
    QVERIFY(!player.audioOutput()->isMuted());

    player.audioOutput()->setMuted(true);
    QVERIFY(player.audioOutput()->isMuted());

    player.audioOutput()->setMuted(false);
    QVERIFY(!player.audioOutput()->isMuted());
}

void TestPlayerBasics::testVolumeProperty()
{
    DragonPlayer player;
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);

    player.audioOutput()->setVolume(0.5);
    QVERIFY(qAbs(player.audioOutput()->volume() - 0.5) < 0.01);

    player.audioOutput()->setVolume(0.0);
    QVERIFY(qAbs(player.audioOutput()->volume()) < 0.01);

    player.audioOutput()->setVolume(1.0);
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);

    player.audioOutput()->setVolume(2.0);
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);
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

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
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
    QSignalSpy spy(player.audioOutput(), &DragonAudioOutput::mutedChanged);

    player.audioOutput()->setMuted(true);
    QVERIFY(player.audioOutput()->isMuted());
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.audioOutput()->setMuted(false);
    QVERIFY(!player.audioOutput()->isMuted());
    QVERIFY(spy.count() > 0);

    spy.clear();
    player.audioOutput()->setMuted(false);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testSetVolume()
{
    DragonPlayer player;
    QSignalSpy spy(player.audioOutput(), &DragonAudioOutput::volumeChanged);

    player.audioOutput()->setVolume(0.5);
    QVERIFY(qAbs(player.audioOutput()->volume() - 0.5) < 0.01);
    QVERIFY(spy.count() > 0);
    QCOMPARE(spy.at(0).at(0).toReal(), 0.5);

    spy.clear();
    player.audioOutput()->setVolume(0.5);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testMutedChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(player.audioOutput(), &DragonAudioOutput::mutedChanged);

    player.audioOutput()->setMuted(true);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);

    spy.clear();
    player.audioOutput()->setMuted(false);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false);

    spy.clear();
    player.audioOutput()->setMuted(false);
    QCOMPARE(spy.count(), 0);
}

void TestPlayerBasics::testVolumeChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(player.audioOutput(), &DragonAudioOutput::volumeChanged);

    player.audioOutput()->setVolume(0.8);
    QTRY_VERIFY(spy.count() > 0);
    QVERIFY(qAbs(spy.at(0).at(0).toReal() - 0.8) < 0.01);
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
    QSignalSpy spy(&player, &DragonPlayer::playbackStateChanged);

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
    DragonSpectrumAnalyzer analyzer(&player);
    QSignalSpy spy(&analyzer, &DragonSpectrumAnalyzer::frameReady);

    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);
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
    DragonSpectrumAnalyzer playingAnalyzer(&playingPlayer);
    PlayerHelper playingHelper(&playingPlayer);
    QSignalSpy playingSpy(&playingAnalyzer, &DragonSpectrumAnalyzer::frameReady);
    playingAnalyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);
    QVERIFY(playingHelper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(playingHelper.playAndWait());
    QTRY_VERIFY_WITH_TIMEOUT(playingSpy.count() > 0, 5000);
    playingPlayer.stop();
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

void TestPlayerBasics::testPauseWhileStoppedWithLoadedMedia()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);

    QVERIFY(helper.playAndWait());
    player.stop();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    // Double-click flow: Elisa re-sets the same source after stop, which
    // early-returns, then issues a pause. QMediaPlayer ignores pause while
    // stopped, so the player must not enter PausedState here.
    player.setSource(QUrl::fromLocalFile(path));
    QTest::qWait(100);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    player.pause();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    // The play/pause button must still work afterwards.
    QVERIFY(helper.playAndWait());
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);

    player.stop();
}

void TestPlayerBasics::testVolumeBoundaryValues()
{
    DragonPlayer player;

    player.audioOutput()->setVolume(0.0);
    QVERIFY(qAbs(player.audioOutput()->volume()) < 0.01);

    player.audioOutput()->setVolume(1.0);
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);

    player.audioOutput()->setVolume(10.0);
    QVERIFY(qAbs(player.audioOutput()->volume() - 1.0) < 0.01);

    player.audioOutput()->setVolume(-1.0);
    QVERIFY(qAbs(player.audioOutput()->volume()) < 0.01);
}

void TestPlayerBasics::testMuteAndVolumeInteraction()
{
    DragonPlayer player;

    player.audioOutput()->setVolume(0.5);
    QVERIFY(qAbs(player.audioOutput()->volume() - 0.5) < 0.01);

    player.audioOutput()->setMuted(true);
    QVERIFY(player.audioOutput()->isMuted());
    QVERIFY(qAbs(player.audioOutput()->volume() - 0.5) < 0.01);

    player.audioOutput()->setMuted(false);
    QVERIFY(!player.audioOutput()->isMuted());
    QVERIFY(qAbs(player.audioOutput()->volume() - 0.5) < 0.01);

    player.audioOutput()->setVolume(0.0);
    player.audioOutput()->setMuted(true);
    QVERIFY(player.audioOutput()->isMuted());
    QVERIFY(qAbs(player.audioOutput()->volume()) < 0.01);

    player.audioOutput()->setMuted(false);
    QVERIFY(!player.audioOutput()->isMuted());
    QVERIFY(qAbs(player.audioOutput()->volume()) < 0.01);
}

void TestPlayerBasics::testPositionTracking()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QCOMPARE(player.position(), 0ms);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    const std::chrono::milliseconds posAfter500ms = player.position();
    QVERIFY2(posAfter500ms > 0ms, "Position must advance during playback");

    QTest::qWait(500);
    const std::chrono::milliseconds posAfter1000ms = player.position();
    QVERIFY2(posAfter1000ms > posAfter500ms, "Position must continue advancing during playback");

    player.stop();
    const std::chrono::milliseconds posAfterStop = player.position();
    QTest::qWait(300);
    QCOMPARE(player.position(), posAfterStop);
}

void TestPlayerBasics::testSeekNoOpWithoutMedia()
{
    DragonPlayer player;
    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.setPosition(5000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);
    QCOMPARE(player.position(), 0ms);

    posSpy.clear();
    player.setPosition(10000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);
    QCOMPARE(player.position(), 0ms);

    posSpy.clear();
    player.setPosition(-100ms);
    QCOMPARE(player.position(), 0ms);

    player.setSource(QUrl::fromLocalFile("/nonexistent.mp3"_L1));
    QTest::qWait(200);

    posSpy.clear();
    player.setPosition(2000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even with invalid source");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);

    posSpy.clear();
    player.setPosition(3000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even with invalid source");
    QCOMPARE(player.position(), 0ms);
}

void TestPlayerBasics::testSeekBehaviorWithValidMedia()
{
    const QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    if (!QFileInfo::exists(path))
        QSKIP("sample-3s.mp3 fixture not available");

    DragonPlayer player;
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY2(player.duration().value_or(0ms) > 0ms, "Duration must be known");
    QVERIFY(player.isSeekable());

    QSignalSpy posSpy(&player, &DragonPlayer::positionChanged);

    player.play();
    QVERIFY2(helper.waitForState(DragonPlayer::PlaybackState::PlayingState, 10000), "Failed to reach PlayingState");

    QTest::qWait(200);
    posSpy.clear();

    player.setPosition(1000ms);
    QTRY_VERIFY_WITH_TIMEOUT(posSpy.count() >= 1, 5000);
    QVERIFY2(player.position() >= 800ms && player.position() <= 1200ms,
             qPrintable(u"Position should be near 1000ms after seek, got %1"_s.arg(player.position().count())));

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
    QVERIFY(player.isSeekable());
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

    player.setPosition(5000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);
    QCOMPARE(player.position(), 0ms);

    posSpy.clear();
    player.setPosition(10000ms);
    QVERIFY2(posSpy.count() >= 1, "setPosition() must emit positionChanged even without media");
    QCOMPARE(posSpy.at(0).at(0).value<std::chrono::milliseconds>(), 0ms);
    QCOMPARE(player.position(), 0ms);
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

void TestPlayerBasics::testFormatChangeRequiredRespectsFlushPending()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    auto *sink = diag.audioSink();
    QVERIFY(sink);

    QVERIFY(diag.isQueueReady());
    QVERIFY(!diag.isFlushPending());

    sink->open(44100, 2);
    QVERIFY(sink->isDeviceOpen());

    // Induce a pending flush (as when seeking while playing)
    sink->setPositionOffset(1000ms, DragonAudioSink::PositionResetMode::Seek);
    QVERIFY(diag.isFlushPending());
    QVERIFY(!diag.isQueueReady());

    // Emit formatChangeRequired from sink. DragonPlayerPrivate::onSinkFormatChangeRequired
    // is connected via Qt::QueuedConnection.
    Q_EMIT sink->formatChangeRequired(48000, 2);
    QCoreApplication::processEvents();

    QVERIFY(sink->hasFormat(48000, 2));
    QVERIFY(diag.isFlushPending());

    // While a flush is pending, the queue MUST remain gated (!isQueueReady).
    // In unpatched code, onSinkFormatChangeRequired called setQueueReady(true) unconditionally.
    QVERIFY2(!diag.isQueueReady(), "Queue must remain gated (!isQueueReady) when flush is pending");

    sink->close();
}

void TestPlayerBasics::testFormatChangeRequiredUngatesQueueWhenNoFlushPending()
{
    DragonPlayer player;
    DragonDiagnostics diag(&player);

    auto *sink = diag.audioSink();
    QVERIFY(sink);

    sink->open(44100, 2);
    QVERIFY(sink->isDeviceOpen());

    // Gate the queue while no flush is pending
    sink->setQueueReady(false);
    QVERIFY(!diag.isQueueReady());
    QVERIFY(!diag.isFlushPending());

    // Emit formatChangeRequired from sink with no pending flush
    Q_EMIT sink->formatChangeRequired(48000, 2);
    QCoreApplication::processEvents();

    QVERIFY(sink->hasFormat(48000, 2));
    QVERIFY(!diag.isFlushPending());
    QVERIFY2(diag.isQueueReady(), "Queue should be un-gated when formatChangeRequired runs with no pending flush");

    sink->close();
}

QTEST_MAIN(TestPlayerBasics)
#include "test_player_basics.moc"
