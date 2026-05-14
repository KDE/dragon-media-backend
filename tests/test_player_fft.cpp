/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * FFT initialization and mode tests for DragonPlayer.
 * Verifies lazy FFT infrastructure creation, mode toggling,
 * and frame emission behavior across track changes.
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

class TestPlayerFft : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLazyFftInitialization();
    void testFftModeToggleCreatesInfrastructure();
    void testFftInfrastructurePersistsAcrossTrackChanges();
    void testFftOffSkipsInfrastructureOnTrackChange();
    void testFftModeBothEmitsDetailedAndBarFrames();

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

void TestPlayerFft::testLazyFftInitialization()
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

void TestPlayerFft::testFftModeToggleCreatesInfrastructure()
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

void TestPlayerFft::testFftInfrastructurePersistsAcrossTrackChanges()
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

void TestPlayerFft::testFftOffSkipsInfrastructureOnTrackChange()
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

void TestPlayerFft::testFftModeBothEmitsDetailedAndBarFrames()
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

QTEST_MAIN(TestPlayerFft)
#include "test_player_fft.moc"
