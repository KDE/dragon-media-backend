/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Prefinish mark and aboutToFinish signal tests for DragonPlayer.
 * Verifies Phonon-compatible behavior for gapless playback preparation.
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

class TestPlayerPrefinish : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPrefinishMarkDefaultValue();
    void testPrefinishMarkGetterSetter();
    void testPrefinishMarkChangedSignal();
    void testPrefinishMarkNoDuplicateSignal();
    void testAboutToFinishNeverEmittedWhenDisabled();

    void testAboutToFinishEmitsOnce();
    void testAboutToFinishTiming();
    void testAboutToFinishResetsOnNewSource();
    void testAboutToFinishResetsOnSeekBack();
    void testAboutToFinishResetsOnGaplessTransition();
    void testPrefinishMarkChangeResets();
    void testShortTrackEmitsImmediately();
    void testStopDuringArmedWindowDisarms();
    void testDisarmMidWindow();

private:
    bool waitForPlaybackStart(DragonPlayer &player, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (player.position() > 0ms) {
                return true;
            }
            QTest::qWait(50);
        }
        return false;
    }
};

void TestPlayerPrefinish::testPrefinishMarkDefaultValue()
{
    DragonPlayer player;
    QCOMPARE(player.prefinishMark(), 2000ms);
}

void TestPlayerPrefinish::testPrefinishMarkGetterSetter()
{
    DragonPlayer player;

    player.setPrefinishMark(1000ms);
    QCOMPARE(player.prefinishMark(), 1000ms);

    player.setPrefinishMark(5000ms);
    QCOMPARE(player.prefinishMark(), 5000ms);

    player.setPrefinishMark(0ms);
    QCOMPARE(player.prefinishMark(), 0ms);
}

void TestPlayerPrefinish::testPrefinishMarkChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::prefinishMarkChanged);

    player.setPrefinishMark(1000ms);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<std::chrono::milliseconds>(), 1000ms);

    player.setPrefinishMark(2000ms);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).value<std::chrono::milliseconds>(), 1000ms);
    QCOMPARE(spy.at(1).at(0).value<std::chrono::milliseconds>(), 2000ms);
}

void TestPlayerPrefinish::testPrefinishMarkNoDuplicateSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::prefinishMarkChanged);

    player.setPrefinishMark(1000ms);
    QCOMPARE(spy.count(), 1);

    player.setPrefinishMark(1000ms);
    QCOMPARE(spy.count(), 1);
}

void TestPlayerPrefinish::testAboutToFinishNeverEmittedWhenDisabled()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(0ms);
    QCOMPARE(player.prefinishMark(), 0ms);

    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QVERIFY2(helper.waitForEndOfMedia(10000), "Track should reach EndOfMedia to fully verify aboutToFinish is never emitted when disabled");

    QCOMPARE(spy.count(), 0);

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishEmitsOnce()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 10000);
    QCOMPARE(spy.count(), 1);
}

void TestPlayerPrefinish::testAboutToFinishTiming()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2000ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    QElapsedTimer timer;
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);
    timer.start();

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 5000);
    qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed >= 700 && elapsed <= 1500,
             qPrintable(u"aboutToFinish should fire at ~1000ms (3000ms track - 2000ms mark), fired at %1ms after playback start"_s.arg(elapsed)));

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishResetsOnNewSource()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 10000);
    player.stop();

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    spy.clear();
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 10000);

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishResetsOnSeekBack()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 5000);

    player.setPosition(0ms);
    QTest::qWait(200);

    QCOMPARE(spy.count(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 2, 5000);

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishResetsOnGaplessTransition()
{
    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy aboutToFinishSpy(&player, &DragonPlayer::aboutToFinish);
    QSignalSpy trackChangedSpy(&player, &DragonPlayer::trackChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.setNextSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(diagnostics);

    QVERIFY2(waitForPlaybackStart(player), "Playback should start producing audio");

    const int underrunsBefore = diagnostics.audioUnderrunCount();

    QTRY_COMPARE(aboutToFinishSpy.count(), 1);

    QTRY_COMPARE(trackChangedSpy.count(), 1);

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during gapless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during gapless transition");

    QTRY_COMPARE_WITH_TIMEOUT(aboutToFinishSpy.count(), 2, 15000);

    VERIFY_AUDIO_ACTIVE(diagnostics);
    const int underrunsAfter = diagnostics.audioUnderrunCount();
    QVERIFY2(underrunsAfter - underrunsBefore == 0,
             qPrintable(u"Audio should not underrun during gapless transition: before=%1, after=%2"_s.arg(underrunsBefore).arg(underrunsAfter)));

    player.stop();
}

void TestPlayerPrefinish::testPrefinishMarkChangeResets()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_COMPARE(spy.count(), 1);

    player.setPosition(0ms);
    QTest::qWait(200);

    spy.clear();

    player.setPrefinishMark(2800ms);

    QTRY_COMPARE(spy.count(), 1);

    player.stop();
}

void TestPlayerPrefinish::testShortTrackEmitsImmediately()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(5000ms);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();

    QVERIFY(waitForPlaybackStart(player, 5000));

    QTest::qWait(300);

    QCOMPARE(spy.count(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 10000);
    QCOMPARE(spy.count(), 1);

    player.stop();
}

void TestPlayerPrefinish::testStopDuringArmedWindowDisarms()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy trackChangedSpy(&player, &DragonPlayer::trackChanged);
    QSignalSpy nextSourceChangedSpy(&player, &DragonPlayer::nextSourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.setNextSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QVERIFY(helper.playAndWait());

    QVERIFY2(waitForPlaybackStart(player), "Playback should start producing audio");

    const int nextSourceChangesBeforeStop = nextSourceChangedSpy.count();
    player.stop();

    QCOMPARE(player.nextSource(), QUrl());
    QVERIFY(nextSourceChangedSpy.count() > nextSourceChangesBeforeStop);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);

    // No handoff may run after the stop: the armed transition, even if its
    // signal was queued before the stop, must abort on the cleared arm.
    QTest::qWait(500);
    QCOMPARE(trackChangedSpy.count(), 0);
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "stop() with a disarmed next source must not reach EndOfMedia");

    // Replaying must restart the current source from the beginning.
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(player.position() > 0ms, 5000);
    QCOMPARE(player.source(), QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QCOMPARE(trackChangedSpy.count(), 0);

    player.stop();
}

void TestPlayerPrefinish::testDisarmMidWindow()
{
    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500ms);
    QSignalSpy trackChangedSpy(&player, &DragonPlayer::trackChanged);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.setNextSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QVERIFY(helper.playAndWait());

    QVERIFY2(waitForPlaybackStart(player), "Playback should start producing audio");

    // Disarm while still inside the prefinish window: the track must end via
    // the normal EndOfMedia path instead of the gapless handoff.
    player.setNextSource(QUrl());
    QCOMPARE(player.nextSource(), QUrl());

    QVERIFY2(helper.waitForEndOfMedia(15000), "Track should reach EndOfMedia after the arm was cleared");
    QCOMPARE(trackChangedSpy.count(), 0);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.stop();
}

QTEST_GUILESS_MAIN(TestPlayerPrefinish)
#include "test_player_prefinish.moc"
