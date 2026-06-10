/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Prefinish mark and aboutToFinish signal tests for DragonPlayer.
 * Verifies Phonon-compatible behavior for gapless playback preparation.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMultimedia/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

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
    void testPrefinishMarkIncreaseResets();
    void testShortTrackEmitsImmediately();

private:
    void skipIfMissing(const QString &filename)
    {
        if (!QFileInfo::exists(TestFixture::fixturePath(filename))) {
            QSKIP(qPrintable(u"Fixture not available: %1"_s.arg(filename)));
        }
    }

    bool waitForPlaybackStart(DragonPlayer &player, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (player.position() > 0) {
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
    QCOMPARE(player.prefinishMark(), 0);
}

void TestPlayerPrefinish::testPrefinishMarkGetterSetter()
{
    DragonPlayer player;

    player.setPrefinishMark(1000);
    QCOMPARE(player.prefinishMark(), 1000);

    player.setPrefinishMark(5000);
    QCOMPARE(player.prefinishMark(), 5000);

    player.setPrefinishMark(0);
    QCOMPARE(player.prefinishMark(), 0);
}

void TestPlayerPrefinish::testPrefinishMarkChangedSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::prefinishMarkChanged);

    player.setPrefinishMark(1000);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 1000);

    player.setPrefinishMark(2000);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).toInt(), 1000);
    QCOMPARE(spy.at(1).at(0).toInt(), 2000);
}

void TestPlayerPrefinish::testPrefinishMarkNoDuplicateSignal()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::prefinishMarkChanged);

    player.setPrefinishMark(1000);
    QCOMPARE(spy.count(), 1);

    player.setPrefinishMark(1000);
    QCOMPARE(spy.count(), 1);
}

void TestPlayerPrefinish::testAboutToFinishNeverEmittedWhenDisabled()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(player.prefinishMark() == 0);

    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTest::qWait(2500);

    QCOMPARE(spy.count(), 0);

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishEmitsOnce()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 10000);
    QCOMPARE(spy.count(), 1);
}

void TestPlayerPrefinish::testAboutToFinishTiming()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2000);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    QElapsedTimer timer;
    timer.start();
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 5000);
    qint64 elapsed = timer.elapsed();

    QVERIFY2(elapsed >= 500 && elapsed <= 2000, qPrintable(u"aboutToFinish should fire at ~1000ms, fired at %1ms"_s.arg(elapsed)));

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishResetsOnNewSource()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500);
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
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 5000);

    player.setPosition(0);
    QTest::qWait(200);

    QCOMPARE(spy.count(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 2, 5000);

    player.stop();
}

void TestPlayerPrefinish::testAboutToFinishResetsOnGaplessTransition()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500);
    QSignalSpy aboutToFinishSpy(&player, &DragonPlayer::aboutToFinish);
    QSignalSpy trackChangedSpy(&player, &DragonPlayer::trackChanged);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.setNextSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));

    player.play();

    QTRY_COMPARE(aboutToFinishSpy.count(), 1);

    QTRY_COMPARE(trackChangedSpy.count(), 1);

    QTRY_COMPARE(aboutToFinishSpy.count(), 2);

    player.stop();
}

void TestPlayerPrefinish::testPrefinishMarkIncreaseResets()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(2500);
    QSignalSpy spy(&player, &DragonPlayer::aboutToFinish);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_COMPARE(spy.count(), 1);

    player.setPosition(0);
    QTest::qWait(200);

    spy.clear();

    player.setPrefinishMark(2500);

    QTRY_COMPARE(spy.count(), 1);

    player.stop();
}

void TestPlayerPrefinish::testShortTrackEmitsImmediately()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    player.setPrefinishMark(5000);
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

QTEST_GUILESS_MAIN(TestPlayerPrefinish)
#include "test_player_prefinish.moc"
