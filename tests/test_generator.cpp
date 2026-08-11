/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests for the std::generator<DecodeEvent> coroutine infrastructure using
 * the production DragonMultimedia event types, plus integration tests that
 * exercise the real DragonDecoder::decodeLoop() generator.
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

#include "decoder/dragondecoder.h"
#include "player/dragonevent.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <generator>
#include <numbers>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

using namespace Qt::StringLiterals;
using namespace DragonMultimedia;

namespace
{
std::generator<DecodeEvent> createTestGenerator(int sampleRate, int channels, qint64 durationMs, const std::vector<std::vector<float>> &sampleBatches)
{
    co_yield FormatReady{sampleRate, channels, durationMs};

    for (const auto &batch : sampleBatches) {
        if (!batch.empty()) {
            co_yield SamplesChunk{.data = std::span<const float>(batch.data(), batch.size()), .sampleRate = sampleRate, .channels = channels};
        }
    }

    co_yield DecodeEof{};
}

std::generator<DecodeEvent> createErrorGenerator(QString errorMessage)
{
    co_yield DecodeError{std::move(errorMessage)};
    co_return;
}

std::generator<DecodeEvent> createErrorMidStreamGenerator()
{
    co_yield FormatReady{44100, 2, 1000};
    co_yield SamplesChunk{};
    co_yield DecodeError{u"Mid-stream error"_s};
    co_yield DecodeEof{};
}

QByteArray createTestWavData(int sampleRate, int channels, int durationMs)
{
    const int numSamples = sampleRate * channels * durationMs / 1000;
    const int dataSize = numSamples * sizeof(int16_t);
    const int totalSize = 44 + dataSize;

    QByteArray wav(totalSize, '\0');
    auto *header = reinterpret_cast<char *>(wav.data());

    auto writeU32 = [&](int offset, quint32 val) {
        std::memcpy(header + offset, &val, 4);
    };
    auto writeU16 = [&](int offset, quint16 val) {
        std::memcpy(header + offset, &val, 2);
    };

    std::memcpy(header, "RIFF", 4);
    writeU32(4, totalSize - 8);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    writeU32(16, 16);
    writeU16(20, 1);
    writeU16(22, static_cast<quint16>(channels));
    writeU32(24, static_cast<quint32>(sampleRate));
    writeU32(28, static_cast<quint32>(sampleRate * channels * 2));
    writeU16(32, static_cast<quint16>(channels * 2));
    writeU16(34, 16);
    std::memcpy(header + 36, "data", 4);
    writeU32(40, static_cast<quint32>(dataSize));

    auto *samples = reinterpret_cast<int16_t *>(header + 44);
    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sampleRate * channels);
        float val = 0.3f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * t);
        samples[i] = static_cast<int16_t>(val * 32767.0f);
    }

    return wav;
}

class MockReadCallback
{
public:
    QByteArray data;
    size_t offset = 0;

    int operator()(std::span<uint8_t> buffer)
    {
        if (offset >= static_cast<size_t>(data.size()))
            return 0;
        size_t toCopy = std::min(buffer.size(), static_cast<size_t>(data.size()) - offset);
        std::memcpy(buffer.data(), data.data() + offset, toCopy);
        offset += toCopy;
        return static_cast<int>(toCopy);
    }
};
}

class TestGeneratorInfrastructure : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFormatReadyFirst();
    void testSamplesChunkOrdering();
    void testDecodeEofLast();
    void testErrorOnlyYieldsError();
    void testSpanDataCopyableBeforeAdvance();
    void testEmptySampleBatches();
    void testErrorMidStream();
    void testMultipleGeneratorsIndependent();
    void testGeneratorPauseAndResume();
    void testProductionDecoderGeneratorEventOrdering();
    void testProductionDecoderGeneratorFormatReadyFromInitialize();
    void testProductionDecoderGeneratorSampleContent();
};

void TestGeneratorInfrastructure::testFormatReadyFirst()
{
    auto gen = createTestGenerator(44100, 2, 3000, {});
    auto it = gen.begin();

    QVERIFY(it != gen.end());

    QVERIFY(std::holds_alternative<FormatReady>(*it));
    auto fr = std::get<FormatReady>(*it);
    QCOMPARE(fr.sampleRate, 44100);
    QCOMPARE(fr.channels, 2);
    QCOMPARE(fr.durationMs, 3000);
}

void TestGeneratorInfrastructure::testSamplesChunkOrdering()
{
    std::vector<std::vector<float>> batches = {{0.1f, 0.2f, 0.3f, 0.4f}, {0.5f, 0.6f, 0.7f, 0.8f}, {0.9f, 1.0f, 1.1f, 1.2f}};

    auto gen = createTestGenerator(48000, 2, 5000, batches);

    int eventCount = 0;
    int samplesSeen = 0;
    bool sawFormat = false;
    bool sawEof = false;

    for (auto event : gen) {
        std::visit(overloaded{[&](auto &&fr) {
                       using T = std::decay_t<decltype(fr)>;
                       if constexpr (std::is_same_v<T, FormatReady>) {
                           QVERIFY(!sawFormat);
                           QVERIFY(!sawEof);
                           sawFormat = true;
                           QCOMPARE(fr.sampleRate, 48000);
                           QCOMPARE(fr.channels, 2);
                       } else if constexpr (std::is_same_v<T, SamplesChunk>) {
                           QVERIFY(sawFormat);
                           QVERIFY(!sawEof);
                           QVERIFY(!fr.data.empty());
                           samplesSeen += static_cast<int>(fr.data.size());
                       } else if constexpr (std::is_same_v<T, DecodeError>) {
                           QFAIL("Should not see error in this test");
                       } else if constexpr (std::is_same_v<T, DecodeEof>) {
                           QVERIFY(sawFormat);
                           QVERIFY(!sawEof);
                           sawEof = true;
                       }
                   }},
                   event);
        ++eventCount;
    }

    QCOMPARE(eventCount, 5);
    QVERIFY(sawFormat);
    QVERIFY(sawEof);
    QCOMPARE(samplesSeen, 12);
}

void TestGeneratorInfrastructure::testDecodeEofLast()
{
    std::vector<std::vector<float>> batches = {{1.0f, 2.0f}};

    auto gen = createTestGenerator(44100, 1, 1000, batches);

    std::optional<DecodeEvent> lastEvent;
    for (auto event : gen) {
        lastEvent = event;
    }

    QVERIFY(lastEvent.has_value());
    QVERIFY(std::holds_alternative<DecodeEof>(*lastEvent));
}

void TestGeneratorInfrastructure::testErrorOnlyYieldsError()
{
    auto gen = createErrorGenerator(u"Test error message"_s);

    int eventCount = 0;
    bool sawError = false;

    for (auto event : gen) {
        QVERIFY(std::holds_alternative<DecodeError>(event));
        auto &err = std::get<DecodeError>(event);
        QCOMPARE(err.message, u"Test error message"_s);
        sawError = true;
        ++eventCount;
    }

    QCOMPARE(eventCount, 1);
    QVERIFY(sawError);
}

void TestGeneratorInfrastructure::testSpanDataCopyableBeforeAdvance()
{
    std::vector<float> batch1 = {1.0f, 2.0f};
    std::vector<float> batch2 = {3.0f, 4.0f};
    std::vector<std::vector<float>> batches = {batch1, batch2};

    auto gen = createTestGenerator(44100, 2, 1000, batches);

    auto it = gen.begin();
    ++it;

    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc1 = std::get<SamplesChunk>(*it);
    std::vector<float> copiedData(sc1.data.begin(), sc1.data.end());
    QCOMPARE(copiedData.size(), 2);
    QCOMPARE(copiedData[0], 1.0f);

    ++it;
    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc2 = std::get<SamplesChunk>(*it);
    QCOMPARE(sc2.data.size(), 2);
    QCOMPARE(sc2.data[0], 3.0f);

    QVERIFY2(copiedData[0] == 1.0f, "Data copied from first span should remain valid after advancing iterator");
}

void TestGeneratorInfrastructure::testEmptySampleBatches()
{
    std::vector<std::vector<float>> batches = {{}, {1.0f, 2.0f}, {}};

    auto gen = createTestGenerator(44100, 2, 1000, batches);

    int samplesEvents = 0;
    int totalSamples = 0;

    for (auto event : gen) {
        std::visit(overloaded{[&](auto &&ev) {
                       using T = std::decay_t<decltype(ev)>;
                       if constexpr (std::is_same_v<T, SamplesChunk>) {
                           if (!ev.data.empty()) {
                               ++samplesEvents;
                               totalSamples += static_cast<int>(ev.data.size());
                           }
                       }
                   }},
                   event);
    }

    QCOMPARE(samplesEvents, 1);
    QCOMPARE(totalSamples, 2);
}

void TestGeneratorInfrastructure::testErrorMidStream()
{
    auto gen = createErrorMidStreamGenerator();

    int eventCount = 0;
    bool sawFormat = false;
    bool sawError = false;
    bool sawEof = false;

    for (auto event : gen) {
        std::visit(overloaded{[&](auto &&ev) {
                       using T = std::decay_t<decltype(ev)>;
                       if constexpr (std::is_same_v<T, FormatReady>) {
                           sawFormat = true;
                       } else if constexpr (std::is_same_v<T, SamplesChunk>) {
                           QVERIFY(sawFormat);
                       } else if constexpr (std::is_same_v<T, DecodeError>) {
                           QVERIFY(sawFormat);
                           sawError = true;
                           QVERIFY(ev.message.contains(u"error"_s));
                       } else if constexpr (std::is_same_v<T, DecodeEof>) {
                           QVERIFY(sawFormat);
                           sawEof = true;
                       }
                   }},
                   event);
        ++eventCount;
    }

    QVERIFY(sawFormat);
    QVERIFY(sawError);
    QVERIFY(sawEof);
    QCOMPARE(eventCount, 4);
}

void TestGeneratorInfrastructure::testMultipleGeneratorsIndependent()
{
    auto gen1 = createTestGenerator(44100, 2, 3000, {{1.0f, 2.0f}});
    auto gen2 = createTestGenerator(48000, 1, 5000, {{3.0f, 4.0f, 5.0f}});

    std::vector<int> rates1, rates2;

    for (auto event : gen1) {
        if (std::holds_alternative<FormatReady>(event)) {
            rates1.push_back(std::get<FormatReady>(event).sampleRate);
        }
    }

    for (auto event : gen2) {
        if (std::holds_alternative<FormatReady>(event)) {
            rates2.push_back(std::get<FormatReady>(event).sampleRate);
        }
    }

    QCOMPARE(rates1.size(), 1);
    QCOMPARE(rates1[0], 44100);
    QCOMPARE(rates2.size(), 1);
    QCOMPARE(rates2[0], 48000);
}

void TestGeneratorInfrastructure::testGeneratorPauseAndResume()
{
    std::vector<std::vector<float>> batches;
    for (int i = 0; i < 5; ++i) {
        batches.push_back({static_cast<float>(i)});
    }

    auto gen = createTestGenerator(44100, 2, 10000, batches);

    auto it = gen.begin();
    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<FormatReady>(*it));

    int processed = 0;
    for (++it; it != gen.end() && processed < 2; ++it, ++processed) {
        QVERIFY(std::holds_alternative<SamplesChunk>(*it));
    }

    QCOMPARE(processed, 2);

    int remaining = 0;
    for (; it != gen.end(); ++it) {
        std::visit(overloaded{[&](auto &&ev) {
                       using T = std::decay_t<decltype(ev)>;
                       if constexpr (std::is_same_v<T, SamplesChunk> || std::is_same_v<T, DecodeEof>) {
                           ++remaining;
                       }
                   }},
                   *it);
    }

    QCOMPARE(remaining, 4);
}

void TestGeneratorInfrastructure::testProductionDecoderGeneratorEventOrdering()
{
    QByteArray wavData = createTestWavData(44100, 2, 200);
    MockReadCallback readCb;
    readCb.data = wavData;

    DragonDecoder decoder(
        [&readCb](std::span<uint8_t> buf) {
            return readCb(buf);
        },
        nullptr,
        wavData.size(),
        QString{});

    auto initResult = decoder.initialize();
    QVERIFY2(initResult.success, qPrintable(initResult.errorMessage));

    std::stop_source stopSource;
    bool sawSamples = false;
    bool sawEof = false;
    bool sawError = false;
    int samplesChunkCount = 0;

    for (auto event : decoder.decodeLoop(stopSource.get_token())) {
        std::visit(overloaded{[&](auto &&ev) {
                       using T = std::decay_t<decltype(ev)>;
                       if constexpr (std::is_same_v<T, FormatReady>) {
                           QFAIL("FormatReady should not be yielded by decodeLoop; it comes from initialize()");
                       } else if constexpr (std::is_same_v<T, SamplesChunk>) {
                           QVERIFY(!sawEof);
                           sawSamples = true;
                           ++samplesChunkCount;
                       } else if constexpr (std::is_same_v<T, DecodeError>) {
                           sawError = true;
                       } else if constexpr (std::is_same_v<T, DecodeEof>) {
                           QVERIFY(!sawEof);
                           sawEof = true;
                       }
                   }},
                   event);
    }

    QVERIFY2(!sawError, "Decoding valid WAV should not produce errors");
    QVERIFY2(samplesChunkCount > 0, "Should have produced at least one SamplesChunk");
    QVERIFY2(sawEof, "Should have seen DecodeEof at end of stream");
    QVERIFY2(sawSamples, "Should have seen sample data before EOF");
}

void TestGeneratorInfrastructure::testProductionDecoderGeneratorFormatReadyFromInitialize()
{
    QByteArray wavData = createTestWavData(48000, 1, 200);
    MockReadCallback readCb;
    readCb.data = wavData;

    DragonDecoder decoder(
        [&readCb](std::span<uint8_t> buf) {
            return readCb(buf);
        },
        nullptr,
        wavData.size(),
        QString{});

    auto initResult = decoder.initialize();

    QVERIFY2(initResult.success, "Initialization should succeed for valid WAV data");
    QCOMPARE(initResult.sampleRate, 48000);
    QCOMPARE(initResult.channels, 1);

    std::stop_source stopSource;
    bool sawFormatReadyInGenerator = false;

    for (auto event : decoder.decodeLoop(stopSource.get_token())) {
        if (std::holds_alternative<FormatReady>(event)) {
            sawFormatReadyInGenerator = true;
        }
    }

    QVERIFY2(!sawFormatReadyInGenerator, "FormatReady should come from initialize(), not from decodeLoop() generator");
}

void TestGeneratorInfrastructure::testProductionDecoderGeneratorSampleContent()
{
    const int sampleRate = 44100;
    const int channels = 1;
    const int durationMs = 200;
    QByteArray wavData = createTestWavData(sampleRate, channels, durationMs);
    MockReadCallback readCb;
    readCb.data = wavData;

    DragonDecoder decoder(
        [&readCb](std::span<uint8_t> buf) {
            return readCb(buf);
        },
        nullptr,
        wavData.size(),
        QString{});

    auto initResult = decoder.initialize();
    QVERIFY2(initResult.success, qPrintable(initResult.errorMessage));

    std::stop_source stopSource;
    std::vector<float> allSamples;

    for (auto event : decoder.decodeLoop(stopSource.get_token())) {
        if (auto *chunk = std::get_if<SamplesChunk>(&event)) {
            allSamples.insert(allSamples.end(), chunk->data.begin(), chunk->data.end());
        }
    }

    QVERIFY2(allSamples.size() > 4000, qPrintable(u"Expected >4000 samples for 200ms of 44100Hz mono, got %1"_s.arg(allSamples.size())));

    bool hasNonZero = std::ranges::any_of(allSamples, [](float s) {
        return std::abs(s) > 1e-6f;
    });
    QVERIFY2(hasNonZero, "Decoded samples should contain non-zero data (sine wave input)");

    double sumSq = 0.0;
    for (float s : allSamples) {
        sumSq += static_cast<double>(s) * static_cast<double>(s);
    }
    double rms = std::sqrt(sumSq / allSamples.size());
    QVERIFY2(rms > 0.15, qPrintable(u"RMS %1 too low for 0.3-amplitude sine wave (expected ~0.212)"_s.arg(rms)));
    QVERIFY2(rms < 0.30, qPrintable(u"RMS %1 too high for 0.3-amplitude sine wave (expected ~0.212)"_s.arg(rms)));

    for (float s : allSamples) {
        QVERIFY2(!std::isnan(s), "Decoded samples should not contain NaN");
        QVERIFY2(!std::isinf(s), "Decoded samples should not contain Inf");
    }
}

QTEST_MAIN(TestGeneratorInfrastructure)
#include "test_generator.moc"
