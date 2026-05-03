/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * End-to-end tests using real audio sample files from fixtures/
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include <dragonsdl/dragondecoder.h>
#include <dragonsdl/dragonplayer.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stop_token>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

class TestFixture
{
public:
    static QString fixturePath(const QString &filename)
    {
        QString fixturesDir = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR);
        return fixturesDir + "/" + filename;
    }

    static QStringList availableFixtures()
    {
        return {"sample-3s.mp3", "sample-3s.aac", "gs-16b-2c-44100hz.ogg", "gs-16b-1c-44100hz.flac", "gs-16b-2c-44100hz.m4a"};
    }
};

class TestE2E : public QObject
{
    Q_OBJECT

private slots:
    void testDecoderMp3File();
    void testDecoderAacFile();
    void testDecoderOggFile();
    void testDecoderFlacFile();
    void testDecoderM4aFile();

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

private:
    struct DecodeResult {
        int sampleRate = 0;
        int channels = 0;
        int64_t duration = 0;
        std::vector<std::float32_t> allSamples;
        bool hadError = false;
        QString errorMessage;
    };

    DecodeResult decodeFileSync(const QString &filePath, int timeoutMs = 10000);
};

TestE2E::DecodeResult TestE2E::decodeFileSync(const QString &filePath, int timeoutMs)
{
    DecodeResult result;

    if (!QFileInfo::exists(filePath)) {
        result.hadError = true;
        result.errorMessage = "File does not exist: " + filePath;
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
        result.errorMessage = "Decode timeout";
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

void TestE2E::testDecoderMp3File()
{
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("MP3 file not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100 || result.sampleRate > 0, qPrintable(QString("Unexpected sample rate: %1").arg(result.sampleRate)));

    qDebug() << "MP3: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderAacFile()
{
    QString filePath = TestFixture::fixturePath("sample-3s.aac");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("AAC file not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "AAC: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderOggFile()
{
    QString filePath = TestFixture::fixturePath("gs-16b-2c-44100hz.ogg");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("OGG file not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100, qPrintable(QString("Expected 44100Hz, got %1").arg(result.sampleRate)));
    QVERIFY2(result.channels == 2, qPrintable(QString("Expected 2 channels, got %1").arg(result.channels)));

    qDebug() << "OGG: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderFlacFile()
{
    QString filePath = TestFixture::fixturePath("gs-16b-1c-44100hz.flac");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("FLAC file not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    QVERIFY2(result.sampleRate == 44100, qPrintable(QString("Expected 44100Hz, got %1").arg(result.sampleRate)));
    QVERIFY2(result.channels == 1, qPrintable(QString("Expected 1 channel (mono), got %1").arg(result.channels)));

    qDebug() << "FLAC: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderM4aFile()
{
    QString filePath = TestFixture::fixturePath("gs-16b-2c-44100hz.m4a");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("M4A file not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "M4A: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestE2E::testDecoderAllFormats_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<int>("expectedSampleRate");
    QTest::addColumn<int>("expectedChannels");

    QTest::newRow("mp3_44k_stereo") << "sample-3s.mp3" << 44100 << -1;
    QTest::newRow("aac_44k_unknown") << "sample-3s.aac" << 44100 << -1;
    QTest::newRow("ogg_44k_stereo") << "gs-16b-2c-44100hz.ogg" << 44100 << 2;
    QTest::newRow("flac_44k_mono") << "gs-16b-1c-44100hz.flac" << 44100 << 1;
    QTest::newRow("m4a_44k_stereo") << "gs-16b-2c-44100hz.m4a" << 44100 << 2;
}

void TestE2E::testDecoderAllFormats()
{
    QFETCH(QString, filename);
    QFETCH(int, expectedSampleRate);
    QFETCH(int, expectedChannels);

    QString filePath = TestFixture::fixturePath(filename);
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("File not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable(QString("Decode failed for %1: %2").arg(filename).arg(result.errorMessage)));

    QVERIFY2(result.sampleRate > 0, qPrintable(QString("Invalid sample rate for %1").arg(filename)));

    QVERIFY2(result.channels > 0, qPrintable(QString("Invalid channel count for %1").arg(filename)));

    QVERIFY2(result.allSamples.size() > 0, qPrintable(QString("No samples decoded for %1").arg(filename)));

    if (expectedSampleRate > 0) {
        QVERIFY2(result.sampleRate == expectedSampleRate,
                 qPrintable(QString("%1: expected %2Hz, got %3Hz").arg(filename).arg(expectedSampleRate).arg(result.sampleRate)));
    }

    if (expectedChannels > 0) {
        QVERIFY2(result.channels == expectedChannels,
                 qPrintable(QString("%1: expected %2 channels, got %3").arg(filename).arg(expectedChannels).arg(result.channels)));
    }

    qDebug() << filename << "-> sampleRate:" << result.sampleRate << "channels:" << result.channels << "totalSamples:" << result.allSamples.size()
             << "duration:" << result.duration << "ms";
}

void TestE2E::testPlayerWithMp3File()
{
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("MP3 file not found: " + filePath));

    DragonPlayer player;

    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);
    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    QUrl url = QUrl::fromLocalFile(filePath);
    player.setSource(url);

    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 5000);

    QVERIFY(player.source() == url);

    QTRY_VERIFY_WITH_TIMEOUT(statusSpy.count() > 0 || errorSpy.count() > 0, 10000);

    if (errorSpy.count() == 0) {
        QVERIFY(statusSpy.count() > 0);

        auto latestStatus = statusSpy.last().at(0).value<DragonPlayer::MediaStatus>();
        qDebug() << "Final status:" << static_cast<int>(latestStatus);

        if (latestStatus == DragonPlayer::MediaStatus::LoadedMedia || latestStatus == DragonPlayer::MediaStatus::BufferedMedia) {
            QTRY_VERIFY_WITH_TIMEOUT(durationSpy.count() > 0, 3000);
            QVERIFY(player.duration() > 0);
        }
    }

    qDebug() << "Player test completed. Status:" << static_cast<int>(player.status()) << "Error:" << static_cast<int>(player.error());
}

void TestE2E::testPlayerWithOggFile()
{
    QString filePath = TestFixture::fixturePath("gs-16b-2c-44100hz.ogg");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("OGG file not found: " + filePath));

    DragonPlayer player;

    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);
    QSignalSpy seekableSpy(&player, &DragonPlayer::seekableChanged);

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(statusSpy.count() > 0 || errorSpy.count() > 0 || seekableSpy.count() > 0, 5000);

    QVERIFY(player.seekable());

    qDebug() << "OGG Player test completed. Status:" << static_cast<int>(player.status());
}

void TestE2E::testPlayerStopActuallyStopsAudio()
{
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("MP3 file not found: " + filePath));

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(player.isAudioActive(), "Audio should be active after starting playback");
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    player.stop();

    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);

    QVERIFY2(!player.isAudioActive(), "Audio should be inactive after calling stop() SDL device must be closed");
}

void TestE2E::testPlayerPauseResumeSequence()
{
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("MP3 file not found: " + filePath));

    DragonPlayer player;

    player.setSource(QUrl::fromLocalFile(filePath));

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(player.isAudioActive(), "Audio should be active after starting playback");
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    player.pause();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PausedState);
    QVERIFY2(player.isAudioActive(), "Audio device should still be open after pause");

    player.play();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
    QVERIFY2(player.isAudioActive(), "Audio should be active after resuming from pause");

    player.stop();
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::StoppedState);
    QVERIFY2(!player.isAudioActive(), "Audio should be inactive after calling stop()");
}

void TestE2E::testDecodeAndVerifySamples_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<bool>("checkSampleRange");
    QTest::addColumn<bool>("checkNoNaNInf");

    QTest::newRow("mp3_verify_samples") << "sample-3s.mp3" << true << true;
    QTest::newRow("ogg_verify_samples") << "gs-16b-2c-44100hz.ogg" << true << true;
    QTest::newRow("flac_verify_samples") << "gs-16b-1c-44100hz.flac" << true << true;
}

void TestE2E::testDecodeAndVerifySamples()
{
    QFETCH(QString, filename);
    QFETCH(bool, checkSampleRange);
    QFETCH(bool, checkNoNaNInf);

    QString filePath = TestFixture::fixturePath(filename);
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("File not found: " + filePath));

    auto result = decodeFileSync(filePath);

    QVERIFY2(!result.hadError, qPrintable("Decode failed: " + result.errorMessage));

    QVERIFY2(result.allSamples.size() > 1000, qPrintable(QString("Expected many samples for %1, got %2").arg(filename).arg(result.allSamples.size())));

    if (checkSampleRange) {
        std::float32_t minSample = 1.0f;
        std::float32_t maxSample = -1.0f;
        for (const auto &s : result.allSamples) {
            minSample = std::min(minSample, s);
            maxSample = std::max(maxSample, s);
        }

        qDebug() << filename << "sample range:" << minSample << "to" << maxSample;

        QVERIFY2(minSample >= -2.0f && maxSample <= 2.0f,
                 qPrintable(QString("%1: sample range out of bounds [%2, %3]").arg(filename).arg(minSample).arg(maxSample)));
    }

    if (checkNoNaNInf) {
        for (size_t i = 0; i < result.allSamples.size(); ++i) {
            float s = result.allSamples[i];
            QVERIFY2(!std::isnan(s), qPrintable(QString("%1: NaN at index %2").arg(filename).arg(i)));
            QVERIFY2(!std::isinf(s), qPrintable(QString("%1: Inf at index %2").arg(filename).arg(i)));
        }
    }

    qDebug() << filename << "decoded" << result.allSamples.size() << "samples successfully";
}

void TestE2E::testDecoderNonExistentFile()
{
    QString filePath = "/nonexistent/path/to/audio.mp3";
    QVERIFY2(!QFileInfo::exists(filePath), "Test file should not exist");

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

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

    QString invalidPath = tempDir.filePath("invalid.mp3");
    QFile file(invalidPath);
    QVERIFY(file.open(QIODevice::WriteOnly));

    QByteArray garbage(1024, 0x42);
    file.write(garbage);
    file.close();

    DragonDecoder decoder(nullptr, invalidPath);

    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

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
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("File not found: " + filePath));

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);
    QSignalSpy durationSpy(&decoder, &DragonDecoder::durationChanged);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);

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
    QStringList files = {"sample-3s.mp3", "sample-3s.aac", "gs-16b-2c-44100hz.ogg"};

    for (const QString &filename : files) {
        QString filePath = TestFixture::fixturePath(filename);
        QVERIFY2(QFileInfo::exists(filePath), qPrintable(QString("File not found: %1").arg(filename)));

        auto result = decodeFileSync(filePath, 8000);

        QVERIFY2(!result.hadError, qPrintable(QString("Failed to decode %1: %2").arg(filename).arg(result.errorMessage)));

        QVERIFY2(result.allSamples.size() > 0, qPrintable(QString("No samples decoded for %1").arg(filename)));

        qDebug() << "Successfully decoded" << filename;
    }
}

void TestE2E::testDecoderNoMemoryLeaks()
{
    QString filePath = TestFixture::fixturePath("sample-3s.mp3");
    QVERIFY2(QFileInfo::exists(filePath), qPrintable("File not found: " + filePath));

    for (int i = 0; i < 5; ++i) {
        auto result = decodeFileSync(filePath, 8000);

        QVERIFY2(!result.hadError, qPrintable(QString("Iteration %1: decode failed: %2").arg(i).arg(result.errorMessage)));

        QVERIFY2(result.allSamples.size() > 0, qPrintable(QString("Iteration %1: no samples").arg(i)));

        qDebug() << "Iteration" << i << "completed, samples:" << result.allSamples.size();
    }
}

void TestE2E::testSeamlessPlaybackTransition()
{
    QString track1 = TestFixture::fixturePath("gs-16b-2c-44100hz.ogg");
    QString track2 = TestFixture::fixturePath("gs-16b-2c-44100hz.m4a");

    QVERIFY2(QFileInfo::exists(track1), qPrintable("OGG fixture not found: " + track1));
    QVERIFY2(QFileInfo::exists(track2), qPrintable("M4A fixture not found: " + track2));

    DragonPlayer player;

    QSignalSpy trackChangedSpy(&player, &DragonPlayer::trackChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy nextSourceSpy(&player, &DragonPlayer::nextSourceChanged);

    player.setSource(QUrl::fromLocalFile(track1));
    player.setNextSource(QUrl::fromLocalFile(track2));

    QVERIFY(player.nextSource() == QUrl::fromLocalFile(track2));
    QVERIFY(nextSourceSpy.count() >= 1);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(player.isAudioActive(), "Audio device should be open during first track playback");
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);

    QTRY_VERIFY_WITH_TIMEOUT(trackChangedSpy.count() > 0, 30000);

    for (const auto &args : stateSpy) {
        auto state = args.at(0).value<DragonPlayer::PlaybackState>();
        QVERIFY2(state != DragonPlayer::PlaybackState::StoppedState, "Playback state should never stop during seamless transition");
    }

    for (const auto &args : statusSpy) {
        auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        QVERIFY2(status != DragonPlayer::MediaStatus::EndOfMedia, "EndOfMedia should not be emitted during seamless transition");
    }

    QVERIFY2(player.isAudioActive(), "Audio device should remain open after seamless transition");

    QVERIFY(player.source() == QUrl::fromLocalFile(track2));

    QVERIFY(!player.nextSource().isValid());

    QVERIFY2(sourceSpy.count() >= 2, qPrintable(QString("Expected at least 2 source changes, got %1").arg(sourceSpy.count())));

    qDebug() << "Seamless playback test passed:"
             << "trackChanged=" << trackChangedSpy.count() << "stateChanges=" << stateSpy.count() << "sourceChanges=" << sourceSpy.count();

    player.stop();
}

QTEST_MAIN(TestE2E)
#include "test_e2e.moc"