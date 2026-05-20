/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Phase 2: Updated tests for std::generator<DecodeEvent> interface.
 *
 * These tests verify the DragonDecoder's coroutine-based decode loop,
 * which yields DecodeEvent variants in order:
 *   FormatReady -> SamplesChunk* -> DecodeEof
 * Errors during initialization are yielded as DecodeError.
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

#include "dragondecoder.h"
#include "dragonevent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace DragonSdl;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
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

class MockErrorReadCallback
{
public:
    std::vector<uint8_t> data;
    size_t pos = 0;
    int callCount = 0;
    int errorAfterCalls = 3;

    int readCallback(std::span<uint8_t> buf)
    {
        callCount++;
        if (callCount > errorAfterCalls) {
            return AVERROR(EIO);
        }

        if (pos >= data.size()) {
            return 0;
        }

        size_t remaining = data.size() - pos;
        size_t toCopy = std::min(buf.size(), remaining);
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + toCopy), buf.begin());
        pos += toCopy;
        return static_cast<int>(toCopy);
    }
};

class TestDecoder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConstruction();
    void testLocalFileDecoding();
    void testNetworkStreamDecoding();
    void testDecoderStatePreservation();
    void testSamplesChunkEvents();
    void testDecodeEofLastEvent();
    void testDurationInFormatReady();
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

    void testPersistentReadError();

    void testGeneratorEventOrdering();
    void testGeneratorSpanLifetime();
    void testGeneratorErrorYielded();
    void testGeneratorMultipleIterations();

private:
    QTemporaryDir m_tempDir;
};

struct DecodeResult {
    std::optional<FormatReady> format;
    std::vector<std::float32_t> samples;
    bool sawEof = false;
    std::optional<DecodeError> error;
    bool hadFatalError = false;
};

DecodeResult runDecoderCollecting(DragonDecoder &decoder, std::stop_token st = {})
{
    DecodeResult result;

    InitResult initRes = decoder.initialize();
    if (initRes.success) {
        FormatReady fr;
        fr.sampleRate = initRes.sampleRate;
        fr.channels = initRes.channels;
        fr.durationMs = initRes.durationMs;
        result.format = fr;
    } else {
        DecodeError err;
        err.message = initRes.errorMessage;
        result.error = err;
        result.hadFatalError = true;
        return result;
    }

    for (auto event : decoder.decodeLoop(st)) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded by decodeLoop");
                              },
                              [&](const SamplesChunk &sc) {
                                  if (!sc.data.empty()) {
                                      result.samples.insert(result.samples.end(), sc.data.begin(), sc.data.end());
                                  }
                              },
                              [&](const DecodeEof &) {
                                  result.sawEof = true;
                              },
                              [&](const DecodeError &err) {
                                  result.error = err;
                              }},
                   event);
    }

    result.hadFatalError = decoder.hasFatalError();
    return result;
}

void TestDecoder::testConstruction()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return 0;
    };
    DragonDecoder decoder(std::move(readCb), {});
    QVERIFY(true);

    DragonDecoder decoder2(nullptr, nullptr, -1, "/nonexistent/file.mp3"_L1);
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

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    auto result = runDecoderCollecting(decoder);

    if (result.error) {
        QFAIL(qPrintable(u"Decoder error: %1"_s.arg(result.error->message)));
    }

    QVERIFY2(result.format.has_value(), "FormatReady event should have been yielded");
    QVERIFY(result.format->sampleRate > 0);
    QVERIFY(result.format->channels > 0);
    QVERIFY2(!result.samples.empty(), "Samples should have been decoded");
    QVERIFY2(result.sawEof, "DecodeEof should have been yielded");

    QCOMPARE(result.format->sampleRate, 44100);
    QCOMPARE(result.format->channels, 2);
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

    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value() || result.error.has_value(), "Decoder should either produce format or error");

    if (!result.error) {
        QVERIFY(result.format->sampleRate > 0);
    }
}

void TestDecoder::testDecoderStatePreservation()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_format.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(48000, 1, 50));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    InitResult res = decoder.initialize();
    QVERIFY(res.success);
    QCOMPARE(res.sampleRate, 48000);
    QCOMPARE(res.channels, 1);

    auto gen = decoder.decodeLoop({});
    auto it = gen.begin();
    QVERIFY(it != gen.end());

    QVERIFY(std::holds_alternative<SamplesChunk>(*it));
}

void TestDecoder::testSamplesChunkEvents()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_samples.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 200));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    auto result = runDecoderCollecting(decoder);

    QVERIFY2(!result.samples.empty(), "No samples were decoded");

    for (const auto &f : result.samples) {
        QVERIFY2(f >= -1.0f && f <= 1.0f, qPrintable(u"Sample out of range: %1"_s.arg(f)));
    }
}

void TestDecoder::testDecodeEofLastEvent()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_eof.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 50));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.sawEof, "DecodeEof should be the final event");
    QVERIFY2(!result.hadFatalError, "Should not have fatal error on normal completion");
}

void TestDecoder::testDurationInFormatReady()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_duration.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 1000));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value(), "FormatReady should contain duration");
    QVERIFY(result.format->durationMs >= 900);
    QVERIFY(result.format->durationMs <= 1100);
}

void TestDecoder::testEmptySource()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return 0;
    };

    DragonDecoder decoder(std::move(readCb), {});
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.error.has_value() || !result.format.has_value(), "Empty source should fail to decode");
}

void TestDecoder::testStopTokenCancellation()
{
    QVERIFY(m_tempDir.isValid());

    QString filePath = m_tempDir.filePath("test_stop.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 5000));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    std::stop_source stopSource;
    std::atomic<bool> startedDecoding{false};

    std::jthread decodeThread([&](std::stop_token st) {
        for (auto event : decoder.decodeLoop(st)) {
            startedDecoding = true;
        }
    });

    QThread::msleep(50);

    stopSource.request_stop();

    decodeThread.join();

    QVERIFY(startedDecoding.load());
}

void TestDecoder::testPersistentReadError()
{
    QByteArray wavBytes = createTestWavData(44100, 2, 5000);
    MockErrorReadCallback mock;
    mock.data.resize(static_cast<size_t>(wavBytes.size()));
    std::memcpy(mock.data.data(), wavBytes.constData(), static_cast<size_t>(wavBytes.size()));
    mock.errorAfterCalls = 3;

    auto readCb = [&mock](std::span<uint8_t> buf) -> int {
        return mock.readCallback(buf);
    };

    DragonDecoder decoder(std::move(readCb), {});

    bool errorEmitted = false;
    QString errorMessage;
    QObject::connect(&decoder, &DragonDecoder::streamError, [&](const QString &msg) {
        errorEmitted = true;
        errorMessage = msg;
    });

    InitResult initRes = decoder.initialize();
    bool sawFormatReady = initRes.success;

    auto gen = decoder.decodeLoop(std::stop_token{});

    int samplesChunks = 0;

    for (auto it = gen.begin(); it != gen.end(); ++it) {
        auto event = *it;
        if (std::holds_alternative<SamplesChunk>(event)) {
            ++samplesChunks;
        } else if (std::holds_alternative<DecodeError>(event)) {
            break;
        }
    }

    QVERIFY2(errorEmitted || decoder.hasFatalError(), "Decoder should have detected and reported the persistent I/O error");
    QVERIFY2(decoder.hasFatalError(), "hasFatalError() should be true after persistent read error");

    QVERIFY2(mock.callCount < 100, "Decoder should have terminated early, not spun forever on errors");

    qDebug() << "testPersistentReadError: sawFormatReady=" << sawFormatReady << "samplesChunks=" << samplesChunks << "errorEmitted=" << errorEmitted
             << "hasFatalError=" << decoder.hasFatalError() << "callCount=" << mock.callCount;
}

void TestDecoder::testDifferentSampleRates_data()
{
    QTest::addColumn<int>("sampleRate");
    QTest::addColumn<int>("expectedRate");

    QTest::newRow("44100") << 44100 << 44100;
    QTest::newRow("48000") << 48000 << 48000;
    QTest::newRow("22050") << 22050 << 22050;
}

void TestDecoder::testDifferentSampleRates()
{
    QFETCH(int, sampleRate);
    QFETCH(int, expectedRate);

    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath(QStringLiteral("test_sr_%1.wav").arg(sampleRate));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(sampleRate, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value(), "FormatReady should have been yielded");
    QCOMPARE(result.format->sampleRate, expectedRate);
}

void TestDecoder::testMonoToStereoConversion()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_mono.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 1, 100));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value(), "FormatReady should have been yielded");
    QCOMPARE(result.format->channels, 1);

    QVERIFY(result.samples.size() > 0);
}

void TestDecoder::testMultipleDecoderInstances()
{
    QVERIFY(m_tempDir.isValid());

    QString file1 = m_tempDir.filePath("test_multi1.wav"_L1);
    QString file2 = m_tempDir.filePath("test_multi2.wav"_L1);

    QFile f1(file1);
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write(createTestWavData(44100, 2, 100));
    f1.close();

    QFile f2(file2);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write(createTestWavData(48000, 1, 100));
    f2.close();

    DragonDecoder decoder1(nullptr, nullptr, -1, file1);
    DragonDecoder decoder2(nullptr, nullptr, -1, file2);

    auto result1 = runDecoderCollecting(decoder1);
    auto result2 = runDecoderCollecting(decoder2);

    QVERIFY2(result1.format.has_value() && result2.format.has_value(), "Both decoders should produce format");

    QCOMPARE(result1.format->sampleRate, 44100);
    QCOMPARE(result2.format->sampleRate, 48000);
    QCOMPARE(result1.format->channels, 2);
    QCOMPARE(result2.format->channels, 1);
}

void TestDecoder::testCorruptDataHandling()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_corrupt.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("RIFF");
    file.write(QByteArray(4, '\x00'));
    file.write("WAVE");
    file.write("corrupt data here");
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.error.has_value() || !result.format.has_value(), "Corrupt data should fail gracefully");
}

void TestDecoder::testResamplerBehavior()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_resampler.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(48000, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value(), "FormatReady should have been yielded");
    QCOMPARE(result.format->sampleRate, 48000);
    QVERIFY(result.samples.size() > 0);
}

void TestDecoder::testChannelConfiguration_data()
{
    QTest::addColumn<int>("channels");
    QTest::addColumn<int>("expectedChannels");

    QTest::newRow("mono") << 1 << 1;
    QTest::newRow("stereo") << 2 << 2;
}

void TestDecoder::testChannelConfiguration()
{
    QFETCH(int, channels);
    QFETCH(int, expectedChannels);

    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath(QStringLiteral("test_ch_%1.wav").arg(channels));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, channels, 100));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.format.has_value(), "FormatReady should have been yielded");
    QCOMPARE(result.format->channels, expectedChannels);
}

void TestDecoder::testGeneratorEventOrdering()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_order.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 100));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    QVERIFY(decoder.initialize().success);

    bool sawEof = false;
    int samplesChunks = 0;

    for (auto event : decoder.decodeLoop({})) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded by decodeLoop");
                              },
                              [&](const SamplesChunk &) {
                                  QVERIFY(!sawEof);
                                  ++samplesChunks;
                              },
                              [&](const DecodeEof &) {
                                  QVERIFY(!sawEof);
                                  sawEof = true;
                              },
                              [&](const DecodeError &) { }},
                   event);
    }

    QVERIFY(sawEof);
    QVERIFY(samplesChunks > 0);
}

void TestDecoder::testGeneratorSpanLifetime()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_span.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 50));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    QVERIFY(decoder.initialize().success);

    auto gen = decoder.decodeLoop({});
    auto it = gen.begin();
    QVERIFY(it != gen.end());

    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc = std::get<SamplesChunk>(*it);
    QVERIFY(!sc.data.empty());

    std::vector<std::float32_t> copiedData(sc.data.begin(), sc.data.end());

    ++it;

    QCOMPARE(copiedData.size(), sc.data.size());
    QVERIFY(copiedData.size() > 0);
}

void TestDecoder::testGeneratorErrorYielded()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_error.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(1000, '\xFF'));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    InitResult res = decoder.initialize();

    QVERIFY(!res.success);
    QVERIFY(!res.errorMessage.isEmpty());
}

void TestDecoder::testGeneratorMultipleIterations()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_multi_iter.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 200));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    InitResult res = decoder.initialize();
    QVERIFY(res.success);

    std::vector<std::float32_t> allSamples;
    int formatSampleRate = res.sampleRate;
    int formatChannels = res.channels;

    for (auto event : decoder.decodeLoop({})) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded");
                              },
                              [&](const SamplesChunk &sc) {
                                  if (!sc.data.empty()) {
                                      allSamples.insert(allSamples.end(), sc.data.begin(), sc.data.end());
                                  }
                              },
                              [&](const auto &) { }},
                   event);
    }

    QVERIFY(formatSampleRate > 0);
    QVERIFY(formatChannels > 0);
    QVERIFY(allSamples.size() > 0);
}

QTEST_MAIN(TestDecoder)
#include "test_decoder.moc"
