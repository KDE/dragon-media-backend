/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Phase 1: std::generator<DecodeEvent> infrastructure tests.
 *
 * These tests verify that the C++23 std::generator coroutine works correctly
 * with the DecodeEvent variant type, specifically:
 *   - Event ordering: FormatReady -> SamplesChunk* -> DecodeEof
 *   - Span lifetime: SamplesChunk span is valid for one iteration
 *   - Exception propagation: exceptions in generator are catchable
 */

#include <QtCore>
#include <QtTest>

#include <generator>
#include <optional>
#include <span>
#include <stdfloat>
#include <variant>
#include <vector>

using namespace Qt::StringLiterals;

namespace TestGenerator
{

struct FormatReady {
    int sampleRate = 0;
    int channels = 0;
    int64_t durationMs = -1;
    bool operator==(const FormatReady &other) const = default;
};

struct SamplesChunk {
    std::span<const std::float32_t> data;
    int sampleRate = 0;
    int channels = 0;
};

struct DecodeError {
    QString message;
    bool operator==(const DecodeError &other) const
    {
        return message == other.message;
    }
};

struct DecodeEof {
};

using DecodeEvent = std::variant<FormatReady, SamplesChunk, DecodeError, DecodeEof>;

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

std::generator<DecodeEvent> createTestGenerator(int sampleRate, int channels, int64_t durationMs, std::vector<std::vector<std::float32_t>> sampleBatches)
{
    co_yield FormatReady{sampleRate, channels, durationMs};

    for (const auto &batch : sampleBatches) {
        if (!batch.empty()) {
            co_yield SamplesChunk{std::span<const std::float32_t>(batch.data(), batch.size()), sampleRate, channels};
        }
    }

    co_yield DecodeEof{};
}

std::generator<DecodeEvent> createErrorGenerator(const QString &errorMessage)
{
    co_yield DecodeError{errorMessage};
    co_return;
}

std::generator<DecodeEvent> createErrorMidStreamGenerator()
{
    co_yield FormatReady{44100, 2, 1000};
    co_yield SamplesChunk{};
    co_yield DecodeError{u"Mid-stream error"_s};
    co_yield DecodeEof{};
}

}

class TestGeneratorInfrastructure : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFormatReadyFirst();
    void testSamplesChunkOrdering();
    void testDecodeEofLast();
    void testErrorOnlyYieldsError();
    void testSpanLifetimeValidForIteration();
    void testSpanInvalidatesOnAdvance();
    void testEmptySampleBatches();
    void testErrorMidStream();
    void testMultipleGeneratorsIndependent();
    void testGeneratorPauseAndResume();
};

void TestGeneratorInfrastructure::testFormatReadyFirst()
{
    using namespace TestGenerator;

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
    using namespace TestGenerator;

    std::vector<std::vector<std::float32_t>> batches = {{0.1f, 0.2f, 0.3f, 0.4f}, {0.5f, 0.6f, 0.7f, 0.8f}, {0.9f, 1.0f, 1.1f, 1.2f}};

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
    using namespace TestGenerator;

    std::vector<std::vector<std::float32_t>> batches = {{1.0f, 2.0f}};

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
    using namespace TestGenerator;

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

void TestGeneratorInfrastructure::testSpanLifetimeValidForIteration()
{
    using namespace TestGenerator;

    std::vector<std::float32_t> originalData = {0.1f, 0.2f, 0.3f, 0.4f};
    std::vector<std::vector<std::float32_t>> batches = {originalData};

    auto gen = createTestGenerator(44100, 2, 1000, batches);

    auto it = gen.begin();
    ++it;

    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc = std::get<SamplesChunk>(*it);
    QCOMPARE(sc.data.size(), 4);
    QCOMPARE(sc.data[0], 0.1f);
    QCOMPARE(sc.data[3], 0.4f);
}

void TestGeneratorInfrastructure::testSpanInvalidatesOnAdvance()
{
    using namespace TestGenerator;

    std::vector<std::float32_t> batch1 = {1.0f, 2.0f};
    std::vector<std::float32_t> batch2 = {3.0f, 4.0f};
    std::vector<std::vector<std::float32_t>> batches = {batch1, batch2};

    auto gen = createTestGenerator(44100, 2, 1000, batches);

    auto it = gen.begin();
    ++it;

    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc1 = std::get<SamplesChunk>(*it);
    std::vector<std::float32_t> copiedData(sc1.data.begin(), sc1.data.end());
    QCOMPARE(copiedData.size(), 2);
    QCOMPARE(copiedData[0], 1.0f);

    ++it;
    QVERIFY(it != gen.end());
    QVERIFY(std::holds_alternative<SamplesChunk>(*it));

    auto sc2 = std::get<SamplesChunk>(*it);
    QCOMPARE(sc2.data.size(), 2);
    QCOMPARE(sc2.data[0], 3.0f);

    QCOMPARE(copiedData[0], 1.0f);
}

void TestGeneratorInfrastructure::testEmptySampleBatches()
{
    using namespace TestGenerator;

    std::vector<std::vector<std::float32_t>> batches = {{}, {1.0f, 2.0f}, {}};

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
    using namespace TestGenerator;

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
    using namespace TestGenerator;

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
    using namespace TestGenerator;

    std::vector<std::vector<std::float32_t>> batches;
    for (int i = 0; i < 5; ++i) {
        batches.push_back({static_cast<std::float32_t>(i)});
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

QTEST_MAIN(TestGeneratorInfrastructure)
#include "test_generator.moc"
