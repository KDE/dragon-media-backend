/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * FFT initialization and mode tests for DragonPlayer + DragonSpectrumAnalyzer.
 * Verifies lazy FFT infrastructure creation, mode toggling,
 * and frame emission behavior across track changes.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMultimedia/dragonfftframe.h>
#include <DragonMultimedia/dragonplayer.h>
#include <DragonMultimedia/dragonspectrumanalyzer.h>

#include <QSignalSpy>
#include <QUrl>

#include <algorithm>

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
    void testFftFrameRateApproaches60Hz();

    void testFftHistoryResetWhenReEnabled();

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
    DragonSpectrumAnalyzer analyzer(&player);
    QCOMPARE(analyzer.mode(), DragonSpectrumAnalyzer::Mode::Off);

    skipIfMissing(u"sample-3s.mp3"_s);

    PlayerHelper helper(&player);
    QSignalSpy fftSpy(&analyzer, &DragonSpectrumAnalyzer::frameReady);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(500);

    QVERIFY2(fftSpy.count() == 0, "FFT frames must not be emitted when mode is Off");

    player.stop();
}

void TestPlayerFft::testFftModeToggleCreatesInfrastructure()
{
    skipIfMissing(u"gs-16b-2c-44100hz.ogg"_s);

    DragonPlayer player;
    DragonSpectrumAnalyzer analyzer(&player);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);

    int frameCount = 0;
    QObject::connect(
        &analyzer,
        &DragonSpectrumAnalyzer::frameReady,
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

    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Off);
    frameCount = 0;
    QTest::qWait(300);
    int framesAfterOff = frameCount;
    QTest::qWait(200);
    int framesAfterMoreWait = frameCount;

    QVERIFY2(framesAfterMoreWait - framesAfterOff < 3, "FFT should stop producing frames when Off");

    frameCount = 0;
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);
    QTest::qWait(500);
    QVERIFY2(frameCount > 0, "FFT frames should resume when re-enabled");

    player.stop();
}

void TestPlayerFft::testFftInfrastructurePersistsAcrossTrackChanges()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s});

    DragonPlayer player;
    DragonSpectrumAnalyzer analyzer(&player);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Both);

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    player.play();

    QSignalSpy fftSpy(&analyzer, &DragonSpectrumAnalyzer::frameReady);
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
    DragonSpectrumAnalyzer analyzer(&player);

    PlayerHelper helper(&player);
    QSignalSpy fftSpy(&analyzer, &DragonSpectrumAnalyzer::frameReady);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());
    QTest::qWait(500);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    player.play();
    QTest::qWait(500);

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::PlayingState);
    QVERIFY2(fftSpy.count() == 0, "FFT frames must not be emitted when mode is Off, even across track changes");
    player.stop();
}

void TestPlayerFft::testFftModeBothEmitsDetailedAndBarFrames()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonSpectrumAnalyzer analyzer(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait(10000));

    QSignalSpy fftSpy(&analyzer, &DragonSpectrumAnalyzer::frameReady);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Both);
    QTRY_VERIFY_WITH_TIMEOUT(fftSpy.size() >= 5, 3000);

    bool sawDetailed = false;
    bool sawBars = false;
    for (int i = 0; i < fftSpy.size(); ++i) {
        QVERIFY(fftSpy.at(i).at(0).isValid());
        auto frame = fftSpy.at(i).at(0).value<DragonFftFrame>();
        if (std::any_of(frame.frequenciesDb.begin(), frame.frequenciesDb.end(), [](float v) {
                return v != 0.0f;
            }))
            sawDetailed = true;
        if (std::any_of(frame.barData.begin(), frame.barData.end(), [](float v) {
                return v != 0.0f;
            }))
            sawBars = true;
    }

    QVERIFY2(sawDetailed, "Both mode should emit frames with frequenciesDb populated");
    QVERIFY2(sawBars, "Both mode should emit frames with barData populated");

    player.stop();
}

void TestPlayerFft::testFftFrameRateApproaches60Hz()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonSpectrumAnalyzer analyzer(&player);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Both);

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));

    // Use a direct-connected atomic counter no event-loop backlog, no QSignalSpy overhead.
    std::atomic<int> frameCount{0};
    QObject::connect(
        &analyzer,
        &DragonSpectrumAnalyzer::frameReady,
        &player,
        [&frameCount]() {
            frameCount.fetch_add(1, std::memory_order_relaxed);
        },
        Qt::DirectConnection);

    QVERIFY(helper.playAndWait());

    // Wait for playback to finish (~3.2 seconds) plus generous drain for trailing frames
    QVERIFY(helper.waitForEndOfMedia(15000));
    QTest::qWait(500);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);

    const int count = frameCount.load(std::memory_order_relaxed);

    // NOTE: In CI the SDL dummy driver decodes faster than real-time, so the
    // wall-clock duration is shorter than the file length. The authoritative
    // rate verification is testFrameCountForThreeSecondsStereo in
    // test_fftprocessor.cpp, which feeds a known sample count directly.
    // This integration test is a sanity check that the full pipeline emits
    // "many" frames (catches severe Bugs like the old ~11 Hz bug).
    QVERIFY2(count >= 120, qPrintable(u"Too few FFT frames (%1) full pipeline severely underproducing (expected ~180 for 3s at 60Hz)"_s.arg(count)));
    QVERIFY2(count <= 220, qPrintable(u"Too many FFT frames (%1) possible burst emission bug"_s.arg(count)));

    player.stop();
}

void TestPlayerFft::testFftHistoryResetWhenReEnabled()
{
    // Regression test: turning FFT Off then On should not reprocess stale
    // sample history, which would cause a CPU spin and spurious frame burst.
    // The bug was in DragonFftPipeline::setMode() not calling reset() on
    // the processor before starting the thread on the Off→On transition.

    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonSpectrumAnalyzer analyzer(&player);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);

    // Track FFT frames across the entire test with a single connection.
    std::atomic<int> totalFrames{0};
    QObject::connect(
        &analyzer,
        &DragonSpectrumAnalyzer::frameReady,
        &player,
        [&totalFrames]() {
            totalFrames.fetch_add(1, std::memory_order_relaxed);
        },
        Qt::AutoConnection);

    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    // Let some frames accumulate to confirm FFT is functioning,
    // but leave enough file duration for the Off→On phase (~1.7s remaining).
    QTest::qWait(800);
    int framesBeforeOff = totalFrames.load(std::memory_order_relaxed);
    QVERIFY2(framesBeforeOff > 10, qPrintable(QString::fromLatin1("Should have produced FFT frames, got %1").arg(framesBeforeOff)));

    // Turn FFT Off, then immediately back On.
    // Some queued frames may arrive from before the mode change.
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Off);

    // Turn FFT back On the file still has ~1.9 seconds remaining.
    // Count only fresh frames from this point.
    totalFrames.store(0, std::memory_order_relaxed);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::BarsOnly);

    // Wait for fresh frames.
    QTest::qWait(800);
    int framesAfterReEnable = totalFrames.load(std::memory_order_relaxed);
    QVERIFY2(framesAfterReEnable > 0, qPrintable(QString::fromLatin1("Should produce frames after re-enabling, got 0")));

    // With the fix, history is reset so frames come from fresh data only.
    // Without the fix, this could be >100 (reprocessing the backlog).
    QVERIFY2(framesAfterReEnable < 80,
             qPrintable(QString::fromLatin1("Expected <80 frames after re-enable (fresh data only), got %1").arg(framesAfterReEnable)));

    player.stop();
}

QTEST_MAIN(TestPlayerFft)
#include "test_player_fft.moc"
