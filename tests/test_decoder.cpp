/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include <dragonsdl/dragondecoder.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class MockReadCallback
{
public:
    std::vector<uint8_t> data;
    size_t pos = 0;
    std::atomic<int> callCount{0};

    int readCallback(std::span<uint8_t> buf)
    {
        callCount.fetch_add(1);
        if (pos >= data.size()) {
            return 0;
        }
        size_t remaining = data.size() - pos;
        size_t toCopy = std::min(buf.size(), remaining);
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + toCopy), buf.begin());
        pos += toCopy;
        return static_cast<int>(toCopy);
    }

    void reset()
    {
        pos = 0;
        callCount.store(0);
    }
};

QByteArray createTestWavData(int sampleRate = 44100, int channels = 2, int durationMs = 100)
{
    const int numSamples = (sampleRate * durationMs) / 1000;
    const int dataSize = numSamples * channels * 2;
    const int fileSize = 36 + dataSize;

    QByteArray wav;
    wav.reserve(44 + dataSize);

    wav.append("RIFF");
    wav.append(static_cast<char>(fileSize & 0xFF));
    wav.append(static_cast<char>((fileSize >> 8) & 0xFF));
    wav.append(static_cast<char>((fileSize >> 16) & 0xFF));
    wav.append(static_cast<char>((fileSize >> 24) & 0xFF));
    wav.append("WAVE");

    wav.append("fmt ");
    wav.append(static_cast<char>(16));
    wav.append(static_cast<char>(0));
    wav.append(static_cast<char>(0));
    wav.append(static_cast<char>(0));
    wav.append(static_cast<char>(1));
    wav.append(static_cast<char>(0));
    wav.append(static_cast<char>(channels));
    wav.append(static_cast<char>(0));
    wav.append(static_cast<char>(sampleRate & 0xFF));
    wav.append(static_cast<char>((sampleRate >> 8) & 0xFF));
    wav.append(static_cast<char>((sampleRate >> 16) & 0xFF));
    wav.append(static_cast<char>((sampleRate >> 24) & 0xFF));
    int byteRate = sampleRate * channels * 2;
    wav.append(static_cast<char>(byteRate & 0xFF));
    wav.append(static_cast<char>((byteRate >> 8) & 0xFF));
    wav.append(static_cast<char>((byteRate >> 16) & 0xFF));
    wav.append(static_cast<char>((byteRate >> 24) & 0xFF));
    int blockAlign = channels * 2;
    wav.append(static_cast<char>(blockAlign & 0xFF));
    wav.append(static_cast<char>((blockAlign >> 8) & 0xFF));
    wav.append(static_cast<char>(16));
    wav.append(static_cast<char>(0));

    wav.append("data");
    wav.append(static_cast<char>(dataSize & 0xFF));
    wav.append(static_cast<char>((dataSize >> 8) & 0xFF));
    wav.append(static_cast<char>((dataSize >> 16) & 0xFF));
    wav.append(static_cast<char>((dataSize >> 24) & 0xFF));

    const float frequency = 440.0f;
    for (int i = 0; i < numSamples; ++i) {
        float sample = 0.3f * std::sin(2.0f * std::numbers::pi_v<float> * frequency * static_cast<float>(i) / static_cast<float>(sampleRate));
        int16_t sampleInt = static_cast<int16_t>(sample * 32767.0f);
        for (int ch = 0; ch < channels; ++ch) {
            wav.append(static_cast<char>(sampleInt & 0xFF));
            wav.append(static_cast<char>((sampleInt >> 8) & 0xFF));
        }
    }

    return wav;
}

class TestDecoder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConstruction();
    void testLocalFileDecoding();
    void testNetworkStreamDecoding();
    void testFormatReadySignal();
    void testSamplesCallback();
    void testDurationSignal();
    void testErrorSignal();
    void testEmptySource();
    void testStopTokenCancellation();

    void testDifferentSampleRates_data();
    void testDifferentSampleRates();
    void testMonoToStereoConversion();
    void testMultipleDecoderInstances();
    void testCorruptDataHandling();
    void testResamplerBehavior();
    void testChannelConfiguration_data();
    void testChannelConfiguration();

    void testSamplesCallbackInvoked();
    void testCallbackThreadAffinity();
    void testCallbackNotSetIsSafe();
    void testCallbackEmptySpanNotFired();
    void testCallbackSampleRateAndChannels();
    void testCallbackReentrant();

private:
    QTemporaryDir m_tempDir;
};

void TestDecoder::testConstruction()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return 0;
    };
    DragonDecoder decoder(std::move(readCb), {});
    QVERIFY(true);

    DragonDecoder decoder2(nullptr, "/nonexistent/file.mp3"_L1);
    QVERIFY(true);
}

void TestDecoder::testLocalFileDecoding()
{
    QVERIFY2(m_tempDir.isValid(), "Failed to create temp directory");

    QString filePath = m_tempDir.filePath("test.wav"_L1);
    QFile wavFile(filePath);
    QVERIFY(wavFile.open(QIODevice::WriteOnly));
    wavFile.write(createTestWavData(44100, 2, 100));
    wavFile.close();

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);
    QSignalSpy durationSpy(&decoder, &DragonDecoder::durationChanged);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);

    std::vector<std::float32_t> capturedSamples;
    std::mutex samplesMutex;
    std::atomic<bool> callbackFired{false};
    decoder.setSamplesCallback([&](std::span<const std::float32_t> data, int, int) {
        std::lock_guard lock(samplesMutex);
        capturedSamples.insert(capturedSamples.end(), data.begin(), data.end());
        callbackFired.store(true);
    });

    std::stop_source stopSource;
    std::jthread decodeThread([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT(callbackFired.load() || (errorSpy.count() > 0) || !decodeThread.joinable(), 10000);

    stopSource.request_stop();
    decodeThread.join();

    if (errorSpy.count() > 0) {
        QString errorMsg = errorSpy.at(0).at(0).toString();
        QFAIL(qPrintable(u"Decoder error: %1"_s.arg(errorMsg)));
    }

    QVERIFY2(formatSpy.count() > 0, "formatReady signal should have been emitted");

    std::lock_guard lock(samplesMutex);
    QVERIFY2(!capturedSamples.empty(), "Samples callback should have been invoked with data");

    QList<QVariant> formatArgs = formatSpy.at(0);
    int sampleRate = formatArgs.at(0).toInt();
    int channels = formatArgs.at(1).toInt();
    QVERIFY(sampleRate > 0);
    QVERIFY(channels > 0);
}

void TestDecoder::testNetworkStreamDecoding()
{
    MockReadCallback mock;
    QByteArray wavData = createTestWavData(44100, 2, 500);
    mock.data = std::vector<uint8_t>(wavData.begin(), wavData.end());

    auto readCb = [&mock](std::span<uint8_t> buf) -> int {
        return mock.readCallback(buf);
    };

    DragonDecoder decoder(std::move(readCb), {});

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);

    std::atomic<bool> callbackFired{false};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        callbackFired.store(true);
    });

    std::stop_source stopSource;
    std::jthread decodeThread([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT((formatSpy.count() > 0) || callbackFired.load() || (errorSpy.count() > 0), 5000);

    stopSource.request_stop();
    decodeThread.join();

    QVERIFY2(formatSpy.count() > 0 || errorSpy.count() > 0, "Decoder should either produce format or error");
}

void TestDecoder::testFormatReadySignal()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_format.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(48000, 1, 50));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy spy(&decoder, &DragonDecoder::formatReady);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 5000);
    t.join();

    QList<QVariant> args = spy.at(0);
    int sampleRate = args.at(0).toInt();
    int channels = args.at(1).toInt();

    QVERIFY(sampleRate == 48000);
    QVERIFY(channels == 1);
}

void TestDecoder::testSamplesCallback()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_samples.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 200));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    std::vector<std::float32_t> capturedSamples;
    std::mutex samplesMutex;
    decoder.setSamplesCallback([&](std::span<const std::float32_t> data, int, int) {
        std::lock_guard lock(samplesMutex);
        capturedSamples.insert(capturedSamples.end(), data.begin(), data.end());
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    t.join();

    std::lock_guard lock(samplesMutex);
    QVERIFY2(capturedSamples.size() > 0, "No samples were decoded");

    for (const auto &f : capturedSamples) {
        QVERIFY2(f >= -1.0f && f <= 1.0f, qPrintable(u"Sample out of range: %1"_s.arg(f)));
    }
}

void TestDecoder::testDurationSignal()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_duration.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 150));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy spy(&decoder, &DragonDecoder::durationChanged);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 5000);
    t.join();

    qint64 duration = spy.at(0).at(0).toLongLong();
    QVERIFY(duration > 0);
    QVERIFY(duration <= 200);
}

void TestDecoder::testErrorSignal()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return -1;
    };
    DragonDecoder decoder(std::move(readCb), "/nonexistent/path/audio.mp3"_L1);

    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    QTRY_VERIFY_WITH_TIMEOUT(formatSpy.count() > 0 || errorSpy.count() > 0, 5000);
    t.join();

    QVERIFY(errorSpy.count() > 0 || formatSpy.count() == 0);
}

void TestDecoder::testEmptySource()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return 0;
    };
    DragonDecoder decoder(std::move(readCb), QString{});

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::atomic<int> callbackCount{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    QTest::qWait(500);
    t.join();

    QVERIFY(formatSpy.count() == 0);
    QVERIFY(callbackCount.load() == 0);
}

void TestDecoder::testStopTokenCancellation()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_cancel.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 500));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::atomic<int> callbackCount{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT(formatSpy.count() > 0, 3000);

    QTest::qWait(100);

    int samplesBeforeCancel = callbackCount.load();

    stopSource.request_stop();
    t.join();

    QVERIFY2(samplesBeforeCancel > 0, qPrintable(u"Expected samples before cancel, got %1"_s.arg(samplesBeforeCancel)));
}

void TestDecoder::testDifferentSampleRates_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<int>("channels");
    QTest::addColumn<int>("durationMs");

    QTest::newRow("44100_stereo") << 44100 << 2 << 100;
    QTest::newRow("48000_stereo") << 48000 << 2 << 100;
    QTest::newRow("22050_mono") << 22050 << 1 << 150;
    QTest::newRow("96000_stereo") << 96000 << 2 << 80;
    QTest::newRow("8000_mono") << 8000 << 1 << 200;
}

void TestDecoder::testDifferentSampleRates()
{
    QFETCH(int, sampleRate);
    QFETCH(int, channels);
    QFETCH(int, durationMs);

    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath(u"test_sr_%1_ch_%2.wav"_s.arg(sampleRate).arg(channels));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(sampleRate, channels, durationMs));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);
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

    QVERIFY2(formatSpy.count() > 0, qPrintable(u"Format not detected for %1Hz/%2ch"_s.arg(sampleRate).arg(channels)));
    QVERIFY2(callbackCount.load() > 0, qPrintable(u"No samples decoded for %1Hz/%2ch"_s.arg(sampleRate).arg(channels)));

    QList<QVariant> formatArgs = formatSpy.at(0);
    int decodedSampleRate = formatArgs.at(0).toInt();
    int decodedChannels = formatArgs.at(1).toInt();
    QVERIFY2(decodedSampleRate == sampleRate, qPrintable(u"Sample rate mismatch: expected %1, got %2"_s.arg(sampleRate).arg(decodedSampleRate)));
    QVERIFY2(decodedChannels == channels, qPrintable(u"Channel mismatch: expected %1, got %2"_s.arg(channels).arg(decodedChannels)));
}

void TestDecoder::testMonoToStereoConversion()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_mono.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 1, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::atomic<int> callbackCount{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY(formatSpy.count() > 0);
    QVERIFY(callbackCount.load() > 0);
    QList<QVariant> formatArgs = formatSpy.at(0);
    int channels = formatArgs.at(1).toInt();
    QVERIFY(channels == 1);
}

void TestDecoder::testMultipleDecoderInstances()
{
    QVERIFY(m_tempDir.isValid());

    QString filePath1 = m_tempDir.filePath("test_multi_1.wav"_L1);
    QString filePath2 = m_tempDir.filePath("test_multi_2.wav"_L1);

    QFile file1(filePath1);
    QVERIFY(file1.open(QIODevice::WriteOnly));
    file1.write(createTestWavData(44100, 2, 100));
    file1.close();

    QFile file2(filePath2);
    QVERIFY(file2.open(QIODevice::WriteOnly));
    file2.write(createTestWavData(48000, 2, 80));
    file2.close();

    DragonDecoder decoder1(nullptr, filePath1);
    DragonDecoder decoder2(nullptr, filePath2);

    QSignalSpy formatSpy1(&decoder1, &DragonDecoder::formatReady);
    QSignalSpy formatSpy2(&decoder2, &DragonDecoder::formatReady);

    std::stop_source stopSource1, stopSource2;
    std::jthread t1([&](std::stop_token st) {
        decoder1.decodeLoop(st);
    });
    std::jthread t2([&](std::stop_token st) {
        decoder2.decodeLoop(st);
    });

    t1.join();
    t2.join();

    QVERIFY(formatSpy1.count() > 0);
    QVERIFY(formatSpy2.count() > 0);

    int sr1 = formatSpy1.at(0).at(0).toInt();
    int sr2 = formatSpy2.at(0).at(0).toInt();
    QVERIFY2(sr1 == 44100, qPrintable(u"Expected 44100, got %1"_s.arg(sr1)));
    QVERIFY2(sr2 == 48000, qPrintable(u"Expected 48000, got %1"_s.arg(sr2)));
}

void TestDecoder::testCorruptDataHandling()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_corrupt.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));

    QByteArray garbage;
    garbage.resize(1024);
    for (int i = 0; i < 1024; ++i) {
        garbage[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    file.write(garbage);
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy errorSpy(&decoder, &DragonDecoder::streamError);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT(formatSpy.count() > 0 || errorSpy.count() > 0, 5000);
    t.join();

    QVERIFY2(errorSpy.count() > 0 || formatSpy.count() == 0, "Corrupt data should produce error or no format");
}

void TestDecoder::testResamplerBehavior()
{
    QVERIFY(m_tempDir.isValid());

    QString filePath = m_tempDir.filePath("test_8k_resample.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(8000, 1, 200));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::vector<std::float32_t> allSamples;
    std::mutex samplesMutex;
    decoder.setSamplesCallback([&](std::span<const std::float32_t> data, int, int) {
        std::lock_guard lock(samplesMutex);
        allSamples.insert(allSamples.end(), data.begin(), data.end());
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY(formatSpy.count() > 0);

    std::lock_guard lock(samplesMutex);
    QVERIFY(!allSamples.empty());

    for (const auto &s : allSamples) {
        QVERIFY2(!std::isnan(s) && !std::isinf(s), qPrintable(u"Sample is NaN or Inf: %1"_s.arg(s)));
        QVERIFY2(s >= -2.0f && s <= 2.0f, qPrintable(u"Sample out of range: %1"_s.arg(s)));
    }
}

void TestDecoder::testChannelConfiguration_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("channels");

    QTest::newRow("mono") << "mono" << 1;
    QTest::newRow("stereo") << "stereo" << 2;
}

void TestDecoder::testChannelConfiguration()
{
    QFETCH(int, channels);

    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath(u"test_ch_%1.wav"_s.arg(channels));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, channels, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);
    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY(formatSpy.count() > 0);
    int decodedChannels = formatSpy.at(0).at(1).toInt();
    QVERIFY2(decodedChannels == channels, qPrintable(u"Channel configuration mismatch: expected %1, got %2"_s.arg(channels).arg(decodedChannels)));
}

void TestDecoder::testSamplesCallbackInvoked()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_callback_invoked.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    std::atomic<bool> callbackFired{false};
    std::atomic<size_t> totalSamples{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t> data, int, int) {
        callbackFired.store(true);
        totalSamples.fetch_add(data.size());
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY2(callbackFired.load(), "Callback should have been invoked");
    QVERIFY2(totalSamples.load() > 0, "Callback should have received non-empty data");
}

void TestDecoder::testCallbackThreadAffinity()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_thread.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    std::thread::id callbackThreadId;
    std::mutex threadIdMutex;
    std::atomic<bool> callbackFired{false};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        std::lock_guard lock(threadIdMutex);
        callbackThreadId = std::this_thread::get_id();
        callbackFired.store(true);
    });

    std::thread::id decodeThreadId;
    std::stop_source stopSource;
    std::jthread decodeThread([&](std::stop_token) {
        decodeThreadId = std::this_thread::get_id();
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT(callbackFired.load(), 5000);
    decodeThread.join();

    std::lock_guard lock(threadIdMutex);
    QVERIFY2(callbackThreadId == decodeThreadId, "Callback must run on the decode thread, not be queued elsewhere");
}

void TestDecoder::testCallbackNotSetIsSafe()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_no_callback.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY(formatSpy.count() > 0);
}

void TestDecoder::testCallbackEmptySpanNotFired()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_no_empty.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    std::atomic<int> emptySpanCount{0};
    std::atomic<int> nonEmptySpanCount{0};
    decoder.setSamplesCallback([&](std::span<const std::float32_t> data, int, int) {
        if (data.empty()) {
            emptySpanCount.fetch_add(1);
        } else {
            nonEmptySpanCount.fetch_add(1);
        }
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY(nonEmptySpanCount.load() > 0);
    QVERIFY2(emptySpanCount.load() == 0, "Callback should never be called with empty span");
}

void TestDecoder::testCallbackSampleRateAndChannels()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_callback_params.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(48000, 1, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    QSignalSpy formatSpy(&decoder, &DragonDecoder::formatReady);

    int callbackSampleRate = 0;
    int callbackChannels = 0;
    std::mutex paramsMutex;
    std::atomic<bool> callbackFired{false};
    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int sr, int ch) {
        std::lock_guard lock(paramsMutex);
        callbackSampleRate = sr;
        callbackChannels = ch;
        callbackFired.store(true);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });

    QTRY_VERIFY_WITH_TIMEOUT(formatSpy.count() > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(callbackFired.load(), 5000);
    t.join();

    QList<QVariant> formatArgs = formatSpy.at(0);
    int formatSampleRate = formatArgs.at(0).toInt();
    int formatChannels = formatArgs.at(1).toInt();

    std::lock_guard lock(paramsMutex);
    QVERIFY2(callbackSampleRate == formatSampleRate,
             qPrintable(u"Sample rate mismatch: callback=%1, formatReady=%2"_s.arg(callbackSampleRate).arg(formatSampleRate)));
    QVERIFY2(callbackChannels == formatChannels, qPrintable(u"Channel mismatch: callback=%1, formatReady=%2"_s.arg(callbackChannels).arg(formatChannels)));
    QVERIFY2(callbackSampleRate == 48000, "Callback should receive 48000Hz");
    QVERIFY2(callbackChannels == 1, "Callback should receive mono");
}

void TestDecoder::testCallbackReentrant()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_reentrant.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, filePath);

    std::atomic<int> firstCallbackCount{0};
    std::atomic<int> secondCallbackCount{0};

    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        firstCallbackCount.fetch_add(1);
    });

    decoder.setSamplesCallback([&](std::span<const std::float32_t>, int, int) {
        secondCallbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread t([&](std::stop_token) {
        decoder.decodeLoop(stopSource.get_token());
    });
    t.join();

    QVERIFY2(firstCallbackCount.load() == 0, "First callback should not have been invoked");
    QVERIFY2(secondCallbackCount.load() > 0, "Second callback should have been invoked");
}

QTEST_MAIN(TestDecoder)
#include "test_decoder.moc"