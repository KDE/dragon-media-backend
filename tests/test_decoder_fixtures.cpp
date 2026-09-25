/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Decoder fixture tests using real audio sample files from fixtures/.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "player/dragonevent.h"
#include <DragonMediaBackend/dragonplayer.h>

#include <algorithm>
#include <cmath>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

struct DecodeResult {
    int sampleRate = 0;
    int channels = 0;
    qint64 duration = 0;
    std::vector<float> allSamples;
    bool hadError = false;
    QString errorMessage;
    bool sawUnexpectedFormatReady = false;
};

DecodeResult decodeFileSync(const QString &filePath, int timeoutMs = 10000)
{
    DecodeResult result;

    if (!QFileInfo::exists(filePath)) {
        result.hadError = true;
        result.errorMessage = u"File does not exist: "_s + filePath;
        return result;
    }

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);

    DragonMediaBackend::InitResult initRes = decoder.initialize();
    if (initRes.success) {
        result.sampleRate = initRes.sampleRate;
        result.channels = initRes.channels;
        result.duration = initRes.duration ? initRes.duration->count() : 0;
    } else {
        result.hadError = true;
        result.errorMessage = initRes.errorMessage;
        return result;
    }

    std::stop_source stopSource;
    std::atomic<bool> decodeComplete{false};
    std::atomic<bool> sawFormatReady{false};

    std::jthread decodeThread(
        [&](std::stop_token st) {
            for (auto event : decoder.decodeLoop(st)) {
                using namespace DragonMediaBackend;
                std::visit(DragonMediaBackend::overloaded{[&sawFormatReady](FormatReady &) {
                                                              sawFormatReady.store(true);
                                                          },
                                                          [&result](SamplesChunk &sc) {
                                                              result.sampleRate = sc.sampleRate;
                                                              result.channels = sc.channels;
                                                              result.allSamples.insert(result.allSamples.end(), sc.data.begin(), sc.data.end());
                                                          },
                                                          [&result](DecodeError &err) {
                                                              result.hadError = true;
                                                              result.errorMessage = err.message;
                                                          },
                                                          [](DecodeEof &) { }},
                           event);
            }
            decodeComplete.store(true);
        },
        stopSource.get_token());

    bool completed = QTest::qWaitFor(
        [&]() {
            return decodeComplete.load() || result.hadError || errorSpy.count() > 0;
        },
        timeoutMs);

    if (!completed) {
        result.hadError = true;
        result.errorMessage = u"Decode timeout"_s;
        stopSource.request_stop();
        decodeThread.join();
        return result;
    }

    stopSource.request_stop();
    decodeThread.join();

    result.sawUnexpectedFormatReady = sawFormatReady.load();

    return result;
}

class TestDecoderFixtures : public QObject
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

    void testDecodeAndVerifySamples_data();
    void testDecodeAndVerifySamples();

    void testDecoderNonExistentFile();
    void testDecoderInvalidFile();

    void testDecoderSignalEventCounts();

    void testDecoderMultipleFilesConsecutive();
    void testDecoderMultipleDecodesNoCrash();

    void testDecodedAudioContentValidation();
    void testSampleCountForKnownDuration();
};

void TestDecoderFixtures::testDecoderMp3File()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.mp3"_s));

    VERIFY_DECODE_SUCCESS(result, u"sample-3s.mp3"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "MP3: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestDecoderFixtures::testDecoderAacFile()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.aac"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.aac"_s));

    VERIFY_DECODE_SUCCESS(result, u"sample-3s.aac"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "AAC: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestDecoderFixtures::testDecoderOggFile()
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

void TestDecoderFixtures::testDecoderFlacFile()
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

void TestDecoderFixtures::testDecoderM4aFile()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.m4a"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-2c-44100hz.m4a"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-2c-44100hz.m4a"_s);
    QVERIFY(result.sampleRate > 0);
    QVERIFY(result.channels > 0);
    QVERIFY(result.allSamples.size() > 0);

    qDebug() << "M4A: sampleRate=" << result.sampleRate << "channels=" << result.channels << "samples=" << result.allSamples.size();
}

void TestDecoderFixtures::testDecoderWmaFile()
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

void TestDecoderFixtures::testDecoderAllFormats_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<int>("expectedSampleRate");
    QTest::addColumn<int>("expectedChannels");

    for (const auto &[filename, sampleRate, channels] : TestFixture::formattedFixtures()) {
        QString rowName = filename.section(u"."_s, 0, 0) + u"_"_s + QString::number(sampleRate);
        QTest::newRow(qPrintable(rowName)) << filename << sampleRate << channels;
    }
}

void TestDecoderFixtures::testDecoderAllFormats()
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

void TestDecoderFixtures::testDecodeAndVerifySamples_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<bool>("checkSampleRange");
    QTest::addColumn<bool>("checkNoNaNInf");

    QTest::newRow("mp3_verify_samples") << u"sample-3s.mp3"_s << true << true;
    QTest::newRow("ogg_verify_samples") << u"gs-16b-2c-44100hz.ogg"_s << true << true;
    QTest::newRow("flac_verify_samples") << u"gs-16b-1c-44100hz.flac"_s << true << true;
}

void TestDecoderFixtures::testDecodeAndVerifySamples()
{
    QFETCH(QString, filename);
    QFETCH(bool, checkSampleRange);
    QFETCH(bool, checkNoNaNInf);

    VERIFY_FIXTURE_EXISTS(filename);
    auto result = decodeFileSync(TestFixture::fixturePath(filename));

    VERIFY_DECODE_SUCCESS(result, filename);
    QVERIFY2(result.allSamples.size() > 1000, qPrintable(u"Expected many samples for %1, got %2"_s.arg(filename).arg(result.allSamples.size())));

    if (checkSampleRange) {
        float minSample = 1.0f;
        float maxSampleVal = -1.0f;
        for (const auto &s : result.allSamples) {
            minSample = std::min(minSample, s);
            maxSampleVal = std::max(maxSampleVal, s);
        }

        qDebug() << filename << "sample range:" << minSample << "to" << maxSampleVal;
        QVERIFY2(minSample >= -1.2f && maxSampleVal <= 1.2f,
                 qPrintable(u"%1: sample range out of bounds [%2, %3]"_s.arg(filename).arg(minSample).arg(maxSampleVal)));

        bool hasNonZero = false;
        for (const auto &s : result.allSamples) {
            if (std::abs(s) > 1e-5f) {
                hasNonZero = true;
                break;
            }
        }
        QVERIFY2(hasNonZero, qPrintable(u"%1: all decoded samples are zero decoder may be broken"_s.arg(filename)));

        float sumSq = 0.0f;
        for (const auto &s : result.allSamples) {
            sumSq += static_cast<float>(s) * static_cast<float>(s);
        }
        float rms = std::sqrt(sumSq / static_cast<float>(result.allSamples.size()));
        QVERIFY2(rms > 0.001f, qPrintable(u"%1: RMS amplitude %2 is suspiciously low audio content not preserved"_s.arg(filename).arg(rms)));
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

void TestDecoderFixtures::testDecoderNonExistentFile()
{
    QString filePath = u"/nonexistent/path/to/audio.mp3"_s;
    QVERIFY2(!QFileInfo::exists(filePath), "Test file should not exist");

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    DragonMediaBackend::InitResult res = decoder.initialize();

    QVERIFY(!res.success);
    QVERIFY(!res.errorMessage.isEmpty());
}

void TestDecoderFixtures::testDecoderInvalidFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    QString invalidPath = tempDir.filePath(u"invalid.mp3"_s);
    QFile file(invalidPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(1024, 0x42));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, invalidPath);
    DragonMediaBackend::InitResult res = decoder.initialize();

    QVERIFY(!res.success);
    QVERIFY(!res.errorMessage.isEmpty());
}

void TestDecoderFixtures::testDecoderSignalEventCounts()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonDecoder decoder(nullptr, nullptr, -1, TestFixture::fixturePath(u"sample-3s.mp3"_s));
    auto errorSpy = SignalSpyHelper::decoderErrorSpy(&decoder);

    QVERIFY(decoder.initialize().success);

    std::atomic<int> samplesChunkCount{0};
    std::atomic<int> samplesCount{0};
    std::atomic<int> eofCount{0};

    std::stop_source stopSource;
    std::jthread t(
        [&](std::stop_token st) {
            for (auto event : decoder.decodeLoop(st)) {
                using namespace DragonMediaBackend;
                std::visit(DragonMediaBackend::overloaded{[&](const FormatReady &) {
                                                              QFAIL("FormatReady should not be yielded");
                                                          },
                                                          [&samplesChunkCount, &samplesCount](SamplesChunk &sc) {
                                                              samplesChunkCount.fetch_add(1);
                                                              samplesCount.fetch_add(static_cast<int>(sc.data.size()));
                                                          },
                                                          [](DecodeError &) {
                                                              QFAIL("DecodeError should not occur when decoding valid file");
                                                          },
                                                          [&eofCount](DecodeEof &) {
                                                              eofCount.fetch_add(1);
                                                          }},
                           event);
            }
        },
        stopSource.get_token());

    t.join();

    QVERIFY2(samplesChunkCount.load() > 0, "Should have received SamplesChunk events");
    QVERIFY2(samplesCount.load() > 0, "Should have decoded actual samples");
    QVERIFY2(eofCount.load() == 1, "Should receive exactly one DecodeEof event");

    qDebug() << "Event counts - samples chunks:" << samplesChunkCount.load() << "total samples:" << samplesCount.load() << "eof:" << eofCount.load()
             << "error:" << errorSpy.count();
}

void TestDecoderFixtures::testDecoderMultipleFilesConsecutive()
{
    for (const QString &filename : TestFixture::allAudioFixtures().mid(0, 3)) {
        VERIFY_FIXTURE_EXISTS(filename);
        auto result = decodeFileSync(TestFixture::fixturePath(filename), 8000);

        VERIFY_DECODE_SUCCESS(result, filename);
        QVERIFY2(result.allSamples.size() > 0, qPrintable(u"No samples decoded for %1"_s.arg(filename)));

        qDebug() << "Successfully decoded" << filename;
    }
}

void TestDecoderFixtures::testDecoderMultipleDecodesNoCrash()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    for (int i = 0; i < 5; ++i) {
        auto result = decodeFileSync(TestFixture::fixturePath(u"sample-3s.mp3"_s), 8000);

        VERIFY_DECODE_SUCCESS(result, QStringLiteral("Iteration %1").arg(i));
        QVERIFY2(result.allSamples.size() > 0, qPrintable(u"Iteration %1: no samples"_s.arg(i)));

        qDebug() << "Iteration" << i << "completed, samples:" << result.allSamples.size();
    }
}

void TestDecoderFixtures::testDecodedAudioContentValidation()
{
    QStringList fixtures = {u"sample-3s.mp3"_s, u"gs-16b-2c-44100hz.ogg"_s, u"gs-16b-1c-44100hz.flac"_s};
    for (const QString &filename : fixtures) {
        VERIFY_FIXTURE_EXISTS(filename);
        auto result = decodeFileSync(TestFixture::fixturePath(filename));

        VERIFY_DECODE_SUCCESS(result, filename);
        QVERIFY2(result.allSamples.size() > 1000, qPrintable(u"%1: expected many samples, got %2"_s.arg(filename).arg(result.allSamples.size())));

        bool hasNonZero = false;
        for (const auto &s : result.allSamples) {
            if (std::abs(s) > 1e-5f) {
                hasNonZero = true;
                break;
            }
        }
        QVERIFY2(hasNonZero, qPrintable(u"%1: all decoded samples are zero"_s.arg(filename)));

        float sumSq = 0.0f;
        for (const auto &s : result.allSamples) {
            sumSq += static_cast<float>(s) * static_cast<float>(s);
        }
        float rms = std::sqrt(sumSq / static_cast<float>(result.allSamples.size()));
        QVERIFY2(rms > 0.001f, qPrintable(u"%1: RMS amplitude %2 too low"_s.arg(filename).arg(rms)));

        qDebug() << filename << "content validation: samples=" << result.allSamples.size() << "rms=" << rms;
    }
}

void TestDecoderFixtures::testSampleCountForKnownDuration()
{
    VERIFY_FIXTURE_EXISTS(u"gs-16b-2c-44100hz.ogg"_s);
    auto result = decodeFileSync(TestFixture::fixturePath(u"gs-16b-2c-44100hz.ogg"_s));

    VERIFY_DECODE_SUCCESS(result, u"gs-16b-2c-44100hz.ogg"_s);

    QVERIFY2(result.duration > 0, qPrintable(u"Duration should be positive, got %1"_s.arg(result.duration)));
    QVERIFY2(result.sampleRate > 0, "Sample rate should be positive");
    QVERIFY2(result.channels > 0, "Channels should be positive");

    const qint64 expectedSamples = result.duration * result.sampleRate * result.channels / 1000;
    const size_t tolerance = static_cast<size_t>(expectedSamples / 10);

    QVERIFY2(result.allSamples.size() >= expectedSamples - tolerance,
             qPrintable(u"Sample count %1 below expected ~%2 (duration=%3ms, sr=%4, ch=%5, tolerance=%6)"_s.arg(result.allSamples.size())
                            .arg(expectedSamples)
                            .arg(result.duration)
                            .arg(result.sampleRate)
                            .arg(result.channels)
                            .arg(tolerance)));
    QVERIFY2(result.allSamples.size() <= expectedSamples + tolerance,
             qPrintable(u"Sample count %1 above expected ~%2 (duration=%3ms, sr=%4, ch=%5, tolerance=%6)"_s.arg(result.allSamples.size())
                            .arg(expectedSamples)
                            .arg(result.duration)
                            .arg(result.sampleRate)
                            .arg(result.channels)
                            .arg(tolerance)));

    qDebug() << "OGG sample count validation: actual=" << result.allSamples.size() << "expected~" << expectedSamples;
}

QTEST_MAIN(TestDecoderFixtures)
#include "test_decoder_fixtures.moc"
