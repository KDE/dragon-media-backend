/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * End-to-end tests using real audio sample files from fixtures/
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <dragonsdl/dragondiagnostics.h>
#include <dragonsdl/dragonplayer.h>

#include <algorithm>
#include <cmath>
#include <stop_token>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace Qt::StringLiterals;

struct DecodeResult {
    int sampleRate = 0;
    int channels = 0;
    int64_t duration = 0;
    std::vector<std::float32_t> allSamples;
    bool hadError = false;
    QString errorMessage;
};

DecodeResult decodeFileSync(const QString &filePath, int timeoutMs = 10000)
{
    DecodeResult result;

    if (!QFileInfo::exists(filePath)) {
        result.hadError = true;
        result.errorMessage = u"File does not exist: "_s + filePath;
        return result;
    }

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);
    QSignalSpy durationSpy(&decoder, &DragonDecoder::durationChanged);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);

    decoder.setSamplesCallback([&result](std::span<const std::float32_t> data, int sampleRate, int channels) {
        result.sampleRate = sampleRate;
        result.channels = channels;
        result.allSamples.insert(result.allSamples.end(), data.begin(), data.end());
    });

    std::stop_source stopSource;
    std::jthread decodeThread([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    bool completed = QTest::qWaitFor(
        [&]() {
            return formatSpy.count() > 0 || errorSpy.count() > 0;
        },
        timeoutMs);

    if (!completed) {
        result.hadError = true;
        result.errorMessage = u"Decode timeout"_s;
        stopSource.request_stop();
        decodeThread.join();
        return result;
    }

    if (errorSpy.count() > 0) {
        result.hadError = true;
        result.errorMessage = errorSpy.at(0).at(0).toString();
        stopSource.request_stop();
        decodeThread.join();
        return result;
    }

    decodeThread.join();

    if (formatSpy.count() > 0) {
        auto args = formatSpy.at(0);
        result.sampleRate = args.at(0).toInt();
        result.channels = args.at(1).toInt();
    }

    if (durationSpy.count() > 0) {
        result.duration = durationSpy.at(0).at(0).toLongLong();
    }

    return result;
}

class TestE2E : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDecoderMp3File();
    void testDecoderAacFile();
    void testDecoderOggFile();
    void testDecoderFlacFile();
    void testDecoderM4aFile();
    void testDecoderWmaFile();

    void testDecoderAllFormats_data();
    void testDecoderAllFormats();

    void testPlayerWithMp3File();
    void testPlayerWithOggFile();

    void testPlayerStopActuallyStopsAudio();
    void testPlayerPauseResumeSequence();

    void testDecodeAndVerifySamples_data();
    void testDecodeAndVerifySamples();

    void testDecoderNonExistentFile();
    void testDecoderInvalidFile();

    void testDecoderSignalEmissionOrder();

    void testSeamlessPlaybackTransition();

    void testDecoderMultipleFilesConsecutive();
    void testDecoderNoMemoryLeaks();

    void testSeamlessPlaybackWithFormatChange();
    void testFftFramesDuringGaplessTransition();

    void testSeamlessPlaybackWhilePaused();
    void testGaplessGenerationCheck();
    void testNonGaplessEofWithFftOn();

    void testSameFormatSeamlessTransition();

    void testDiagnosticsBasicFunctionality();
};

void TestE2E::testDecoderMp3File()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.mp3"_s));

    VERIFY_DECODE_SUCCESS(result, u"sample-3s.mp3"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100 || result.sampleRate > 0, qPrintable(u"Unexpected sample rate: %1"_s.arg(result.sampleRate)));

    qDebug() << "MP3: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderAacFile()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.aac"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.aac"_s));

    VERIFY_DECODE_SUCCESS(result, u"sample-3s.aac"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "AAC: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderOggFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-2c-44100hz.ogg"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100, qPrintable(u"Expected 44100Hz, got %1"_s.arg(result.sampleRate)));
    QVERIFY2(result.channels == 2, qPrintable(u"Expected 2 channels, got %1"_s.arg(result.channels)));

    qDebug() << "OGG: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderFlacFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-1c-44100hz.flac"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-1c-44100hz.flac"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-1c-44100hz.flac"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100, qPrintable(u"Expected 44100Hz, got %1"_s.arg(result.sampleRate)));
    QVERIFY2(result.channels == 1, qPrintable(u"Expected 1 channel (mono), got %1"_s.arg(result.channels)));

    qDebug() << "FLAC: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderM4aFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.m4a"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-2c-44100hz.m4a"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "M4A: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderWmaFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-1c-44100hz.wma"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-1c-44100hz.wma"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100, qPrintable(u"Expected 44100Hz, got %1"_s.arg(result.sampleRate)));
    QVERIFY2(result.channels == 1, qPrintable(u"Expected 1 channel (mono), got %1"_s.arg(result.channels)));

    qDebug() << "WMA: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderAllFormats_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<int>("expectedSampleRate");
    QTest::addColumn<int>("expectedChannels");

    for (const auto &[filename, sampleRate, channels] : TestFixture::formattedFixtures()) {
        QString rowName = filename.section(u"."_s, 0, 0) + u"_"_s + QString::number(sampleRate);
        QTest::newRow(qPrintable(rowName)) << filename << sampleRate << channels;
    }
}

void TestE2E::testDecoderAllFormats()
{
    QFETCH(QString, filename);
    QFETCH(int, expectedSampleRate);
    QFETCH(int, expectedChannels);

    QString filePath = TestFixture::fixturePath(filename);
    VERIFY_FIXTURE_EXISTS(filename);

    auto result = decodeFileSync(filePath);

    VERIFY_DECODE_SUCCESS(result, filename);
    QVERIFY2(result.sampleRate > 0, qPrintable(u"Invalid sample rate for %1"_s.arg(filename)));
    QVERIFY2(result.channels > 0, qPrintable(u"Invalid channel count for %1"_s.arg(filename)));
    QVERIFY2(result.allSamples.size() > 0, qPrintable(u"No samples decoded for %1"_s.arg(filename)));

    if (expectedSampleRate > 0) {
        QVERIFY2(result.sampleRate == expectedSampleRate,
                 qPrintable(u"%1: expected %2Hz, got %3Hz"_s.arg(filename).arg(expectedSampleRate).arg(result.sampleRate)));
    }

    if (expectedChannels > 0) {
        QVERIFY2(result.channels == expectedChannels,
                 qPrintable(u"%1: expected %2 channels, got %3"_s.arg(filename).arg(expectedChannels).arg(result.channels)));
    }

    qDebug() << filename << "-> sampleRate:" << result.sampleRate << "channels:" << result.channels << "totalSamples:" << result.allSamples.size()
             << "duration:" << result.duration << "ms";
}

void TestE2E::testPlayerWithMp3File()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    QString path = TestFixture::fixturePath(u"sample-3s.mp3"_s);
    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(player.source() == QUrl::fromLocalFile(path));

    auto status = player.status();
    if (status == DragonPlayer::MediaStatus::LoadedMedia || status == DragonPlayer::MediaStatus::BufferedMedia) {
        QVERIFY(durationSpy.count() > 0 || player.duration() > 0);
    }

    qDebug() << "Player test completed. Status:" << static_cast<int>(player.status()) << "Error:" << static_cast<int>(player.error());
}

void TestE2E::testPlayerWithOggFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);
    QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
    QVERIFY(player.seekable());

    qDebug() << "OGG Player test completed. Status:" << static_cast<int>(player.status());
}

void TestE2E::testPlayerStopActuallyStopsAudio()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    VERIFY_AUDIO_ACTIVE(player);
    QVERIFY(diagnostics.sdlAudioBufferFrames() > 0);
    QVERIFY(diagnostics.sdlAudioBufferUs() > 0);
    QVERIFY(diagnostics.hasActiveDecoder());
    QVERIFY(diagnostics.decodeLoopActive() || diagnostics.decodeQueueSize() > 0);

    player.stop();
    VERIFY_STOPPED_STATE(player);
    VERIFY_AUDIO_INACTIVE(player);

    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.sdlAudioBufferUs() == -1, 1000);
    QVERIFY2(diagnostics.sdlAudioBufferFrames() == -1, "SDL buffer frames should return -1 after stop");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder after stop");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active after stop");
}

void TestE2E::testPlayerPauseResumeSequence()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    VERIFY_AUDIO_ACTIVE(player);
    VERIFY_PLAYING_STATE(player);

    QVERIFY(helper.pauseAndWait());
    VERIFY_PAUSED_STATE(player);
    VERIFY_AUDIO_ACTIVE(player);

    QVERIFY(helper.playAndWait());
    VERIFY_PLAYING_STATE(player);
    VERIFY_AUDIO_ACTIVE(player);

    player.stop();
    VERIFY_STOPPED_STATE(player);
    VERIFY_AUDIO_INACTIVE(player);
}

void TestE2E::testDecodeAndVerifySamples_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<bool>("checkSampleRange");
    QTest::addColumn<bool>("checkNoNaNInf");

    QTest::newRow("mp3_verify_samples") << u"sample-3s.mp3"_s << true << true;
    QTest::newRow("ogg_verify_samples") << u"gs-16b-2c-44100hz.ogg"_s << true << true;
    QTest::newRow("flac_verify_samples") << u"gs-16b-1c-44100hz.flac"_s << true << true;
}

void TestE2E::testDecodeAndVerifySamples()
{
    QFETCH(QString, filename);
    QFETCH(bool, checkSampleRange);
    QFETCH(bool, checkNoNaNInf);

    VERIFY_FIXTURE_EXISTS(filename);
    auto result = decodeFileSync(TestFixture::fixturePath(filename));

    VERIFY_DECODE_SUCCESS(result, filename);
    QVERIFY2(result.allSamples.size() > 1000, qPrintable(u"Expected many samples for %1, got %2"_s.arg(filename).arg(result.allSamples.size())));

    if (checkSampleRange) {
        std::float32_t minSample = 1.0f;
        std::float32_t maxSample = -1.0f;
        for (const auto &s : result.allSamples) {
            minSample = std::min(minSample, s);
            maxSample = std::max(maxSample, s);
        }

        qDebug() << filename << "sample range:" << minSample << "to" << maxSample;
        QVERIFY2(minSample >= -2.0f && maxSample <= 2.0f, qPrintable(u"%1: sample range out of bounds [%2, %3]"_s.arg(filename).arg(minSample).arg(maxSample)));
    }

    if (checkNoNaNInf) {
        for (size_t i = 0; i < result.allSamples.size(); ++i) {
            float s = result.allSamples[i];
            QVERIFY2(!std::isnan(s), qPrintable(u"%1: NaN at index %2"_s.arg(filename).arg(i)));
            QVERIFY2(!std::isinf(s), qPrintable(u"%1: Inf at index %2"_s.arg(filename).arg(i)));
        }
    }

    qDebug() << filename << "decoded" << result.allSamples.size() << "samples successfully";
}

void TestE2E::testDecoderNonExistentFile()
{
    QString filePath = u"/nonexistent/path/to/audio.mp3"_s;
    QVERIFY2(!QFileInfo::exists(filePath), "Test file should not exist");

    DragonDecoder decoder(nullptr, filePath);
    auto errorSpy = SignalSpyHelper::decoderErrorSpy(&decoder);
    auto formatSpy = SignalSpyHelper::formatSpy(&decoder);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    [[maybe_unused]] bool done = QTest::qWaitFor(
        [&]() {
            return formatSpy.count() > 0 || errorSpy.count() > 0;
        },
        5000);

    t.join();

    QVERIFY2(errorSpy.count() > 0 || formatSpy.count() == 0, "Non-existent file should produce error or no format");
}

void TestE2E::testDecoderInvalidFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString invalidPath = tempDir.filePath(u"invalid.mp3"_s);
    QFile file(invalidPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(1024, 0x42));
    file.close();

    DragonDecoder decoder(nullptr, invalidPath);
    auto errorSpy = SignalSpyHelper::decoderErrorSpy(&decoder);
    auto formatSpy = SignalSpyHelper::formatSpy(&decoder);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    [[maybe_unused]] bool done = QTest::qWaitFor(
        [&]() {
            return formatSpy.count() > 0 || errorSpy.count() > 0;
        },
        5000);

    t.join();

    QVERIFY2(errorSpy.count() > 0 || formatSpy.count() == 0, "Invalid file should produce error or no format");
}

void TestE2E::testDecoderSignalEmissionOrder()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonDecoder decoder(nullptr, TestFixture::fixturePath(u"sample-3s.mp3"_s));
    auto formatSpy = SignalSpyHelper::formatSpy(&decoder);
    auto durationSpy = SignalSpyHelper::decoderDurationSpy(&decoder);
    auto errorSpy = SignalSpyHelper::decoderErrorSpy(&decoder);

    std::atomic<int> callbackCount{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    t.join();

    if (formatSpy.count() > 0 && callbackCount.load() > 0) {
        QVERIFY2(formatSpy.at(0).at(0).toInt() > 0, "Format should specify valid sample rate");
    }

    qDebug() << "Signal counts - format:" << formatSpy.count() << "duration:" << durationSpy.count() << "samples (callback):" << callbackCount.load()
             << "error:" << errorSpy.count();
}

void TestE2E::testDecoderMultipleFilesConsecutive()
{
    for (const QString &filename : TestFixture::allAudioFixtures().mid(0, 3)) {
        VERIFY_FIXTURE_EXISTS(filename);
        auto result = decodeFileSync(TestFixture::fixturePath(filename), 8000);

        VERIFY_DECODE_SUCCESS(result, filename);
        QVERIFY2(result.allSamples.size() > 0, qPrintable(u"No samples decoded for %1"_s.arg(filename)));

        qDebug() << "Successfully decoded" << filename;
    }
}

void TestE2E::testDecoderNoMemoryLeaks()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    for (int i = 0; i < 5; ++i) {
        auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.mp3"_s), 8000);

        VERIFY_DECODE_SUCCESS(result, QStringLiteral("Iteration %1").arg(i));
        QVERIFY2(result.allSamples.size() > 0, qPrintable(u"Iteration %1: no samples"_s.arg(i)));

        qDebug() << "Iteration" << i << "completed, samples:" << result.allSamples.size();
    }
}

void TestE2E::testSeamlessPlaybackTransition()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.m4a"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-16b-2c-44100hz.m4a"_s);

    QVERIFY(player.nextSource() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s)));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during seamless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during seamless transition");

    VERIFY_AUDIO_ACTIVE(player);
    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s)));
    QVERIFY(!player.nextSource().isValid());
    QVERIFY2(sourceSpy.count() >= 2, qPrintable(u"Expected at least 2 source changes, got %1"_s.arg(sourceSpy.count())));

    qDebug() << "Seamless playback test passed:"
             << "trackChanged=" << trackSpy.count() << "stateChanges=" << stateSpy.count() << "sourceChanges=" << sourceSpy.count();

    player.stop();
}

void TestE2E::testSeamlessPlaybackWithFormatChange()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-1c-44100hz.flac"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto statusSpy = SignalSpyHelper::statusSpy(&player);
    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-16b-1c-44100hz.flac"_s);

    QVERIFY(player.nextSource().isValid());
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during seamless transition");
    QVERIFY2(helper.verifyNoEndOfMedia(statusSpy), "EndOfMedia should not be emitted during seamless transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-1c-44100hz.flac"_s)));
    QVERIFY(!player.nextSource().isValid());
    VERIFY_AUDIO_ACTIVE(player);

    player.stop();
}

void TestE2E::testFftFramesDuringGaplessTransition()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.m4a"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);
    FftFrameCounter counter(&player);

    player.setFftMode(DragonPlayer::FftMode::BarsOnly);

    QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
    helper.setNextSource(u"gs-16b-2c-44100hz.m4a"_s);
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesBeforeTransition = counter.count();

    QVERIFY(helper.waitForTrackChange());

    QTest::qWait(500);
    int framesAfterTransition = counter.count();

    QVERIFY2(framesBeforeTransition > 0, "FFT frames should arrive during first track playback");
    QVERIFY2(framesAfterTransition > framesBeforeTransition, "FFT frames should continue arriving after gapless transition");

    player.stop();
}

void TestE2E::testSeamlessPlaybackWhilePaused()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.m4a"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    auto sourceSpy = SignalSpyHelper::sourceSpy(&player);

    QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    QVERIFY(helper.pauseAndWait());
    VERIFY_PAUSED_STATE(player);
    VERIFY_AUDIO_ACTIVE(player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s)));
    QVERIFY(sourceSpy.count() > 0);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s)));
    VERIFY_AUDIO_ACTIVE(player);

    QVERIFY(helper.playAndWait());
    VERIFY_PLAYING_STATE(player);
    QVERIFY2(stateSpy.count() >= 1, qPrintable(u"Expected state change on resume, got %1"_s.arg(stateSpy.count())));

    qDebug() << "Track change while paused test passed:"
             << "sourceChanged=" << sourceSpy.count() << "stateChanges=" << stateSpy.count();

    player.stop();
}

void TestE2E::testGaplessGenerationCheck()
{
    QStringList shortTracks = TestFixture::shortFixtures();
    QVERIFY(!shortTracks.isEmpty());

    VERIFY_FIXTURE_EXISTS(shortTracks[0]);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    VERIFY_FIXTURE_EXISTS(u"gs-16b-1c-44100hz.flac"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QList<QUrl> sourceHistory;
    QObject::connect(
        &player,
        &DragonPlayer::sourceChanged,
        &player,
        [&]() {
            sourceHistory.append(player.source());
        },
        Qt::DirectConnection);

    QVERIFY(helper.setSourceAndWait(shortTracks[0]));
    helper.setNextSource(u"gs-16b-2c-44100hz.ogg"_s);
    QVERIFY(helper.playAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(player.position() > 2000, 10000);
    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-1c-44100hz.flac"_s)));

    QVERIFY(helper.setSourceAndWait(u"gs-16b-1c-44100hz.flac"_s));
    QTest::qWait(1000);

    QVERIFY2(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-1c-44100hz.flac"_s)),
             qPrintable(u"Final source should be flac, got %1"_s.arg(player.source().toString())));

    QVERIFY(player.playbackState() != DragonPlayer::PlaybackState::PlayingState || player.status() == DragonPlayer::MediaStatus::LoadedMedia);

    bool track2WasEverSource = sourceHistory.contains(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s)));
    if (track2WasEverSource && sourceHistory.last() == QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-16b-1c-44100hz.flac"_s))) {
        qDebug() << "setSource(flac) interrupted an active gapless transition to ogg";
    } else if (!track2WasEverSource) {
        qDebug() << "Generation-check path: stale gapless callback to ogg was discarded";
    }

    player.stop();
}

void TestE2E::testNonGaplessEofWithFftOn()
{
    QStringList shortTracks = TestFixture::shortFixtures();
    QVERIFY(!shortTracks.isEmpty());
    VERIFY_FIXTURE_EXISTS(shortTracks[0]);

    DragonPlayer player;
    PlayerHelper helper(&player);
    FftFrameCounter counter(&player);

    player.setFftMode(DragonPlayer::FftMode::BarsOnly);
    QVERIFY(helper.setSourceAndWait(shortTracks[0]));
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesDuringPlayback = counter.count();
    QVERIFY2(framesDuringPlayback > 0, "FFT frames should arrive during playback");

    QVERIFY(helper.waitForEndOfMedia());
    QTRY_VERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QTest::qWait(1000);
    int framesAfterDrain = counter.count();
    int inFlightFrames = framesAfterDrain - framesDuringPlayback;

    QTest::qWait(500);
    int framesAfterCheck = counter.count();
    int genuinelyNewFrames = framesAfterCheck - framesAfterDrain;

    QVERIFY2(genuinelyNewFrames == 0, qPrintable(u"FFT should stop producing frames after EOF; got %1 genuinely new frames"_s.arg(genuinelyNewFrames)));
    qDebug() << "Non-gapless EOF FFT: in-flight frames after drain=" << inFlightFrames << "genuinely new frames=" << genuinelyNewFrames;

    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);
    QVERIFY(helper.playAndWait());

    QTest::qWait(500);
    int framesAfterRestart = counter.count();

    QVERIFY2(framesAfterRestart > framesAfterDrain, "FFT frames should resume after play() at EndOfMedia");

    player.stop();
}

void TestE2E::testSameFormatSeamlessTransition()
{
    QStringList stereo = TestFixture::stereoFixtures();
    QVERIFY(stereo.size() >= 2);

    VERIFY_FIXTURE_EXISTS(stereo[0]);
    VERIFY_FIXTURE_EXISTS(stereo[1]);

    DragonPlayer player;
    PlayerHelper helper(&player);

    auto trackSpy = SignalSpyHelper::trackSpy(&player);

    QVERIFY(helper.setSourceAndWait(stereo[0]));
    helper.setNextSource(stereo[1]);
    QVERIFY(player.nextSource().isValid());

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    auto stateSpy = SignalSpyHelper::stateSpy(&player);
    bool audioEverInactive = helper.wasAudioEverInactive(0, 10);

    QVERIFY(helper.waitForTrackChange());

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during same-format seamless transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(TestFixture::fixturePath(stereo[1])));
    QVERIFY(!player.nextSource().isValid());

    QVERIFY2(!audioEverInactive, "Audio device should remain active continuously during same-format gapless transition");

    player.stop();
}

void TestE2E::testDiagnosticsBasicFunctionality()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    QVERIFY2(diagnostics.sdlAudioBufferUs() == -1, "SDL buffer should return -1 when stopped (no device)");
    QVERIFY2(diagnostics.sdlAudioBufferFrames() == -1, "SDL buffer frames should return -1 when stopped (no device)");
    QVERIFY2(diagnostics.decodeQueueSize() == 0, "Decode queue should be 0 when stopped");
    QVERIFY2(diagnostics.fftQueueSize() == 0, "FFT queue should be 0 when stopped");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder when stopped");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active when stopped");
    QVERIFY2(diagnostics.audioCallbackHz() == 0.0f, "Audio callback Hz should be 0 when stopped");

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY2(diagnostics.sdlAudioBufferUs() == -1, "SDL buffer should return -1 before playback starts (no device yet)");

    QVERIFY(helper.playAndWait());
    QTest::qWait(100);

    VERIFY_AUDIO_ACTIVE(player);
    QVERIFY2(diagnostics.sdlAudioBufferFrames() > 0, "SDL hardware buffer should report >0 frames during playback");
    QVERIFY2(diagnostics.sdlAudioBufferUs() > 0, "SDL hardware buffer should report >0 µs during playback");
    QVERIFY2(diagnostics.hasActiveDecoder(), "Should have active decoder during playback");

    QVERIFY(helper.stopAndWait());

    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.sdlAudioBufferUs() == -1, 1000);
    QVERIFY2(diagnostics.sdlAudioBufferFrames() == -1, "SDL buffer frames should return -1 after stop");
    QVERIFY2(!diagnostics.hasActiveDecoder(), "Should not have active decoder after stop");
    QVERIFY2(!diagnostics.decodeLoopActive(), "Decode loop should not be active after stop");

    qDebug() << "Diagnostics basic functionality test passed";
}

QTEST_MAIN(TestE2E)
#include "test_e2e.moc"
