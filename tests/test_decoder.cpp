/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "decoder/dragondecoder.h"
#include "player/dragonevent.h"

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
using namespace DragonMediaBackend;
using namespace std::chrono_literals;

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
    void testMonoFileStaysMono();
    void testMultipleDecoderInstances();
    void testCorruptDataHandling();
    void testPassthroughAt48k();
    void testChannelConfiguration_data();
    void testChannelConfiguration();

    void testPersistentReadError();

    void testGeneratorEventOrdering();
    void testGeneratorSpanLifetime();
    void testConcurrentChunksDoNotAlias();
    void testGeneratorErrorYielded();
    void testGeneratorMultipleIterations();

    void testDecodedSineWaveContent();
    void testSampleCountValidation();
    void testPerChunkMetadata();

private:
    QTemporaryDir m_tempDir;
};

struct DecodeResult {
    std::optional<FormatReady> format;
    std::vector<float> samples;
    bool sawEof = false;
    std::optional<DecodeError> error;
    bool hadFatalError = false;
    int chunkSampleRate = 0;
    int chunkChannels = 0;
};

DecodeResult runDecoderCollecting(DragonDecoder &decoder, std::stop_token st = {})
{
    DecodeResult result;

    InitResult initRes = decoder.initialize();
    if (initRes.success) {
        FormatReady fr;
        fr.sampleRate = initRes.sampleRate;
        fr.channels = initRes.channels;
        fr.duration = initRes.duration;
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
                                      if (sc.sampleRate > 0)
                                          result.chunkSampleRate = sc.sampleRate;
                                      if (sc.channels > 0)
                                          result.chunkChannels = sc.channels;
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
    QVERIFY(!decoder.hasFatalError());

    DragonDecoder decoder2(nullptr, nullptr, -1, "/nonexistent/file.mp3"_L1);
    QVERIFY(!decoder2.hasFatalError());
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
    QVERIFY(result.format->duration >= 900ms);
    QVERIFY(result.format->duration <= 1100ms);
}

void TestDecoder::testEmptySource()
{
    auto readCb = [](std::span<uint8_t>) -> int {
        return 0;
    };

    DragonDecoder decoder(std::move(readCb), {});
    auto result = runDecoderCollecting(decoder);

    QVERIFY2(result.error.has_value(), "Empty source should produce a decode error");
}

void TestDecoder::testStopTokenCancellation()
{
    QVERIFY(m_tempDir.isValid());

    QString filePath = m_tempDir.filePath("test_stop.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 30000));
    file.close();

    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray wavData = file.readAll();
    file.close();
    QVERIFY(!wavData.isEmpty());

    struct SlowReadCallback {
        QByteArray data;
        size_t offset = 0;
        std::atomic<bool> stopRequested{false};

        int operator()(std::span<uint8_t> buf)
        {
            if (stopRequested.load())
                return 0;
            if (offset >= static_cast<size_t>(data.size()))
                return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (stopRequested.load())
                return 0;
            size_t toCopy = std::min(buf.size(), static_cast<size_t>(data.size()) - offset);
            std::memcpy(buf.data(), data.data() + offset, toCopy);
            offset += toCopy;
            return static_cast<int>(toCopy);
        }
    };

    SlowReadCallback readCb;
    readCb.data = wavData;

    DragonDecoder decoder(
        [&readCb](std::span<uint8_t> buf) {
            return readCb(buf);
        },
        nullptr,
        wavData.size(),
        QString{});

    QVERIFY(decoder.initialize().success);

    std::atomic<bool> startedDecoding{false};
    std::atomic<bool> cancelledEarly{false};

    std::jthread decodeThread([&](std::stop_token st) {
        for (auto event : decoder.decodeLoop(st)) {
            startedDecoding = true;
        }
        cancelledEarly.store(st.stop_requested());
    });

    QTRY_VERIFY_WITH_TIMEOUT(startedDecoding.load(), 5000);

    readCb.stopRequested.store(true);
    decodeThread.get_stop_source().request_stop();

    decodeThread.join();

    QVERIFY(startedDecoding.load());
    QVERIFY2(cancelledEarly.load(), "Decode thread should have been cancelled via jthread stop_token, not finished naturally");
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

void TestDecoder::testMonoFileStaysMono()
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

    QVERIFY2(result.error.has_value(), "Corrupt data should produce a decode error");
}

void TestDecoder::testPassthroughAt48k()
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
    file.write(createTestWavData(44100, 2, 200));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    QVERIFY(decoder.initialize().success);

    auto gen = decoder.decodeLoop({});
    auto it = gen.begin();

    // Walk to the first SamplesChunk; ignore FormatReady etc.
    while (it != gen.end() && !std::holds_alternative<SamplesChunk>(*it)) {
        ++it;
    }
    QVERIFY(it != gen.end());

    auto sc = std::get<SamplesChunk>(*it);
    QVERIFY(!sc.data.empty());

    // Snapshot the bytes; the span must keep pointing at these same bytes.
    const std::vector<float> snapshot(sc.data.begin(), sc.data.end());

    // Advance far enough that a decoder reusing a shared scratch buffer
    // would have overwritten the bytes this span points at.
    constexpr int kAdvances = 8;
    for (int i = 0; i < kAdvances && it != gen.end(); ++i) {
        ++it;
    }

    QVERIFY2(std::equal(sc.data.begin(), sc.data.end(), snapshot.begin()), "SamplesChunk::data must remain valid after the generator is advanced");
}

void TestDecoder::testConcurrentChunksDoNotAlias()
{
    QVERIFY(m_tempDir.isValid());
    QString filePath = m_tempDir.filePath("test_alias.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(44100, 2, 200));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    QVERIFY(decoder.initialize().success);

    auto gen = decoder.decodeLoop({});
    auto it = gen.begin();

    auto nextChunk = [&]() -> std::optional<SamplesChunk> {
        while (it != gen.end()) {
            if (std::holds_alternative<SamplesChunk>(*it)) {
                auto chunk = std::get<SamplesChunk>(*it);
                ++it; // advance past the chunk we just consumed
                if (!chunk.data.empty()) {
                    return chunk;
                }
                continue;
            }
            ++it;
        }
        return std::nullopt;
    };

    auto first = nextChunk();
    QVERIFY(first.has_value());

    auto second = nextChunk();
    QVERIFY(second.has_value());

    // Both chunks are alive simultaneously; the decoder must not have
    // handed the same underlying buffer to both.
    QVERIFY2(first->data.data() != second->data.data(), "Concurrently live SamplesChunks must not share underlying storage");
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

    QVERIFY2(!res.success, "Initialization should fail for corrupt data");
    QVERIFY2(!res.errorMessage.isEmpty(), "Error message should be non-empty for failed initialization");
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

    QCOMPARE(res.sampleRate, 44100);
    QCOMPARE(res.channels, 2);

    std::vector<float> firstPassSamples;

    for (auto event : decoder.decodeLoop({})) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded");
                              },
                              [&](const SamplesChunk &sc) {
                                  if (!sc.data.empty()) {
                                      firstPassSamples.insert(firstPassSamples.end(), sc.data.begin(), sc.data.end());
                                  }
                              },
                              [&](const auto &) { }},
                   event);
    }

    QVERIFY2(firstPassSamples.size() > 0, "First pass should produce samples");

    std::vector<float> secondPassSamples;
    DragonDecoder decoder2(nullptr, nullptr, -1, filePath);
    QVERIFY2(decoder2.initialize().success, "Second decoder instance should initialize successfully");

    for (auto event : decoder2.decodeLoop({})) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded");
                              },
                              [&](const SamplesChunk &sc) {
                                  if (!sc.data.empty()) {
                                      secondPassSamples.insert(secondPassSamples.end(), sc.data.begin(), sc.data.end());
                                  }
                              },
                              [&](const auto &) { }},
                   event);
    }

    QVERIFY2(secondPassSamples.size() > 0, "Second pass should produce samples");
    QCOMPARE_EQ(firstPassSamples.size(), secondPassSamples.size());
}

void TestDecoder::testDecodedSineWaveContent()
{
    QVERIFY(m_tempDir.isValid());

    constexpr int sampleRate = 44100;
    constexpr int channels = 2;
    constexpr int durationMs = 200;
    constexpr float frequency = 440.0f;
    constexpr float amplitude = 0.3f;

    QString filePath = m_tempDir.filePath("test_sine_content.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(sampleRate, channels, durationMs));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    if (result.error) {
        QFAIL(qPrintable(u"Decoder error: %1"_s.arg(result.error->message)));
    }

    QVERIFY2(!result.samples.empty(), "Should have decoded samples");

    bool hasNonZero = false;
    for (const auto &s : result.samples) {
        if (std::abs(s) > 1e-5f) {
            hasNonZero = true;
            break;
        }
    }
    QVERIFY2(hasNonZero, "Decoded samples should not all be zero");

    float sumSq = 0.0f;
    for (const auto &s : result.samples) {
        sumSq += static_cast<float>(s) * static_cast<float>(s);
    }
    float rms = std::sqrt(sumSq / static_cast<float>(result.samples.size()));

    const float expectedRms = amplitude / std::sqrt(2.0f);
    QVERIFY2(rms > expectedRms * 0.8f && rms < expectedRms * 1.2f,
             qPrintable(u"RMS amplitude %1 does not match expected ~%2 for a %3-amplitude sine"_s.arg(rms).arg(expectedRms).arg(amplitude)));

    constexpr size_t N = 4096;
    if (result.samples.size() >= N * static_cast<size_t>(channels)) {
        std::vector<float> mono;
        mono.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += result.samples[i * static_cast<size_t>(channels) + c];
            }
            mono.push_back(sum / static_cast<float>(channels));
        }

        float maxMag = 0.0f;
        size_t maxBin = 0;
        for (size_t k = 1; k < N / 2; ++k) {
            float re = 0.0f, im = 0.0f;
            for (size_t n = 0; n < N; ++n) {
                const float angle = -2.0f * std::numbers::pi_v<float> * static_cast<float>(k) * static_cast<float>(n) / static_cast<float>(N);
                re += mono[n] * std::cos(angle);
                im += mono[n] * std::sin(angle);
            }
            float mag = std::sqrt(re * re + im * im);
            if (mag > maxMag) {
                maxMag = mag;
                maxBin = k;
            }
        }

        const float detectedFreq = static_cast<float>(maxBin) * static_cast<float>(sampleRate) / static_cast<float>(N);
        QVERIFY2(std::abs(detectedFreq - frequency) < 50.0f,
                 qPrintable(u"FFT peak at %1 Hz, expected ~%2 Hz (bin %3)"_s.arg(detectedFreq).arg(frequency).arg(maxBin)));
    }
}

void TestDecoder::testSampleCountValidation()
{
    QVERIFY(m_tempDir.isValid());

    constexpr int sampleRate = 44100;
    constexpr int channels = 2;
    constexpr int durationMs = 500;

    QString filePath = m_tempDir.filePath("test_sample_count.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(sampleRate, channels, durationMs));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);
    auto result = runDecoderCollecting(decoder);

    if (result.error) {
        QFAIL(qPrintable(u"Decoder error: %1"_s.arg(result.error->message)));
    }

    QVERIFY2(result.format.has_value(), "FormatReady should have been yielded");
    QCOMPARE(result.format->sampleRate, sampleRate);
    QCOMPARE(result.format->channels, channels);

    const qint64 expectedSamples = static_cast<qint64>(sampleRate) * channels * durationMs / 1000;
    const size_t tolerance = static_cast<size_t>(expectedSamples / 10);

    QVERIFY2(result.samples.size() >= expectedSamples - tolerance,
             qPrintable(u"Sample count %1 is below expected ~%2 (tolerance %3)"_s.arg(result.samples.size()).arg(expectedSamples).arg(tolerance)));
    QVERIFY2(result.samples.size() <= expectedSamples + tolerance,
             qPrintable(u"Sample count %1 is above expected ~%2 (tolerance %3)"_s.arg(result.samples.size()).arg(expectedSamples).arg(tolerance)));
}

void TestDecoder::testPerChunkMetadata()
{
    QVERIFY(m_tempDir.isValid());

    constexpr int sampleRate = 44100;
    constexpr int channels = 2;
    constexpr int durationMs = 200;

    QString filePath = m_tempDir.filePath("test_chunk_meta.wav"_L1);
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(createTestWavData(sampleRate, channels, durationMs));
    file.close();

    DragonDecoder decoder(nullptr, nullptr, -1, filePath);

    InitResult initRes = decoder.initialize();
    QVERIFY2(initRes.success, "Initialize should succeed");

    int chunkCount = 0;
    for (auto event : decoder.decodeLoop({})) {
        std::visit(overloaded{[&](const FormatReady &) {
                                  QFAIL("FormatReady should not be yielded by decodeLoop");
                              },
                              [&](const SamplesChunk &sc) {
                                  if (!sc.data.empty()) {
                                      ++chunkCount;
                                      QCOMPARE(sc.sampleRate, initRes.sampleRate);
                                      QCOMPARE(sc.channels, initRes.channels);
                                  }
                              },
                              [&](const DecodeEof &) { },
                              [&](const DecodeError &) { }},
                   event);
    }

    QVERIFY2(chunkCount > 0, "Should have received at least one SamplesChunk");
}

QTEST_MAIN(TestDecoder)
#include "test_decoder.moc"
