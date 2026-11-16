/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "logging_timestamp_init.h"

using namespace Qt::StringLiterals;

#include "fft/fftprocessor.h"
#include "pipe_test_utils.h"
#include "player/pipe.h"
#include <DragonFftFrame>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <stop_token>
#include <thread>
#include <vector>

namespace
{
bool isFrequenciesEmpty(const DragonFftFrame &frame)
{
    return std::all_of(frame.frequencies().begin(), frame.frequencies().end(), [](float v) {
        return v == 0.0f;
    });
}

bool isBarEmpty(const DragonFftFrame &frame)
{
    return std::all_of(frame.bars().begin(), frame.bars().end(), [](float v) {
        return v == 0.0f;
    });
}

bool allFinite(std::span<const float> values)
{
    return std::all_of(values.begin(), values.end(), [](float v) {
        return std::isfinite(v);
    });
}

// The processor no longer stores a "latest frame": tests observe frames through
// the production setFrameCallback() path instead. The callback runs on the
// thread that runs processLoop(), so the captured frames are guarded by a mutex
// while the invocation count stays atomic for quiescence polling.
class FrameCapture
{
public:
    void capture(DragonFftFrame frame)
    {
        std::scoped_lock lock(m_mutex);
        m_frames.push_back(std::move(frame));
        frameCount.fetch_add(1, std::memory_order_relaxed);
    }

    // The most recently emitted frame, or a null frame if none was emitted yet.
    [[nodiscard]] DragonFftFrame latest() const
    {
        std::scoped_lock lock(m_mutex);
        return m_frames.empty() ? DragonFftFrame() : m_frames.back();
    }

    [[nodiscard]] std::vector<DragonFftFrame> frames() const
    {
        std::scoped_lock lock(m_mutex);
        return m_frames;
    }

    std::atomic<int> frameCount{0};

private:
    mutable std::mutex m_mutex;
    std::vector<DragonFftFrame> m_frames;
};

// Writes blocks that deliberately carry no in-band format metadata. The
// processor must ignore these.
size_t writeBlocksWithoutFormat(Pipe<PcmBlock>::Producer producer, std::span<const float> data)
{
    const size_t blocksNeeded = (data.size() + PcmBlock::MAX_SAMPLES - 1) / PcmBlock::MAX_SAMPLES;
    return producer.writeSomeWith(blocksNeeded, [&](std::span<PcmBlock> b1, std::span<PcmBlock> b2) {
        size_t srcOffset = 0;
        size_t blockIdx = 0;
        auto fillDst = [&](std::span<PcmBlock> dst) {
            for (size_t i = 0; i < dst.size() && srcOffset < data.size(); ++i, ++blockIdx) {
                const size_t toCopy = std::min(data.size() - srcOffset, PcmBlock::MAX_SAMPLES);
                dst[i].count = toCopy;
                dst[i].pts = std::chrono::microseconds(blockIdx * 1000);
                // sampleRate and channels stay at their default of 0.
                for (size_t j = 0; j < toCopy; ++j, ++srcOffset) {
                    dst[i].samples[j] = data[srcOffset];
                }
            }
        };
        fillDst(b1);
        fillDst(b2);
    });
}

// processLoop() runs on the jthread's own stop token so that the thread is
// still stopped and joined when an assertion fails and the test returns early.
[[nodiscard]] std::jthread startProcessorThread(FftProcessor &processor)
{
    return std::jthread([&processor](std::stop_token st) {
        processor.processLoop(st);
    });
}

// The processor emits its first frame only once a block carrying a valid
// format has arrived, which can take a pipe wait cycle; wait for it instead
// of only waiting for the frame counter to stop moving.
[[nodiscard]] bool waitForFirstFrame(const std::atomic<int> &frameCount, int timeoutMs = 5000)
{
    return QTest::qWaitFor(
        [&frameCount]() {
            return frameCount.load(std::memory_order_relaxed) > 0;
        },
        timeoutMs);
}
}

class TestFftProcessor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testHannWindow_data();
    void testHannWindow();

    void testConstruction();
    void testSetQueue();
    void testReset();

    void testProcessLoopSineWave();
    void testProcessLoopSilence();
    void testProcessLoopMultipleFrames();
    void testFrameCallbackInvoked();

    void testPeakHoldDecay();
    void testBarDataSizeValidation();
    void testFftProducesOutputAboveThreshold_data();
    void testFftProducesOutputAboveThreshold();
    void testFrameTimestamp();

    void testFftModeOff();
    void testFftModeBarsOnly();
    void testFftModeDetailedOnly();
    void testFftModeSwitch();
    void testFrameCountForThreeSecondsStereo();
    void testConfigurableRate();

    void testFftResumesAfterModeToggle();
    void testFftStopTokenHonoredInInnerLoop();

    void testFftFrequencyLocalization();
    void testFftMultiTonePeaks();

    void testInBandFormatSwitchAtBlockBoundary();

private:
    std::vector<float> createSineWave(float frequency, int sampleRate, int numSamples);
    std::vector<float> createSilence(int numSamples);
};

void TestFftProcessor::testHannWindow_data()
{
    QTest::addColumn<int>("windowSize");

    QTest::newRow("small") << 64;
    QTest::newRow("standard") << 4096;
    QTest::newRow("large") << 8192;
}

void TestFftProcessor::testHannWindow()
{
    QFETCH(int, windowSize);

    std::vector<float> data(static_cast<size_t>(windowSize), 1.0f);
    FftProcessor::applyHannWindow(data);

    QVERIFY(data.front() == 0.0f);
    QVERIFY(data.back() == 0.0f);

    for (int i = 0; i < windowSize / 2; ++i) {
        float diff = std::abs(data[static_cast<size_t>(i)] - data[static_cast<size_t>(windowSize - 1 - i)]);
        QVERIFY2(diff < 1e-6f,
                 qPrintable(QString("Window not symmetric at index %1: %2 vs %3"_L1)
                                .arg(i)
                                .arg(data[static_cast<size_t>(i)])
                                .arg(data[static_cast<size_t>(windowSize - 1 - i)])));
    }

    for (const auto &val : data) {
        QVERIFY2(val >= 0.0f && val <= 1.0f, qPrintable(QString("Window value out of range: %1"_L1).arg(val)));
    }
}

void TestFftProcessor::testConstruction()
{
    Pipe<PcmBlock> pipe(256);

    // A freshly constructed processor has no format yet, so it must not emit
    // any frames before the first valid in-band block arrives.
    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QTest::qWait(100);
    processorThread.request_stop();
    processorThread.join();

    QVERIFY2(capture.frameCount.load() == 0, "A processor without format metadata should emit no frames");
    QVERIFY(isFrequenciesEmpty(capture.latest()));
    QVERIFY(isBarEmpty(capture.latest()));
}

void TestFftProcessor::testSetQueue()
{
    Pipe<PcmBlock> pipe(256);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    // Blocks without in-band format metadata are consumed but ignored: the
    // processor has no sample rate or channel count to work with.
    auto sineWave = createSineWave(440.0f, 44100, static_cast<int>(FftProcessor::FFT_SIZE));
    const auto written = writeBlocksWithoutFormat(pipe.producer(), sineWave);
    QVERIFY2(written > 0, "writeBlocksWithoutFormat wrote zero blocks pipe may be full or data empty");

    auto processorThread = startProcessorThread(processor);

    QTest::qWait(300);
    processorThread.request_stop();
    processorThread.join();

    QVERIFY2(capture.frameCount.load() == 0, "Blocks without valid format metadata should be ignored");
    QVERIFY(isFrequenciesEmpty(capture.latest()));
    QVERIFY(isBarEmpty(capture.latest()));
}

void TestFftProcessor::testReset()
{
    Pipe<PcmBlock> pipe(256);
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(440.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    const auto written = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks");

    auto processorThread = startProcessorThread(processor);

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return capture.frameCount.load(std::memory_order_relaxed) > 0;
                 },
                 5000),
             "Should have produced at least one frame");
    const int framesBeforeReset = quiescencePolling(capture.frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    QVERIFY2(framesBeforeReset > 0, "Should have frequency data before reset");
    QVERIFY2(!isFrequenciesEmpty(capture.latest()), "Should have frequency data before reset");
    QVERIFY2(!isBarEmpty(capture.latest()), "Should have bar data before reset");

    // reset() drops the accumulated history and forgets the format. Feeding
    // less than one FFT window of new samples must therefore not produce a
    // frame: the format has to be re-established by the first valid block and
    // the history rebuilt from scratch.
    processor.reset();

    std::vector<float> halfWindow = createSineWave(440.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE) / 2);
    const auto writtenAfterReset = writeBlocks(pipe.producer(), halfWindow, sampleRate, channels);
    QVERIFY2(writtenAfterReset > 0, "writeBlocks wrote zero blocks");

    std::jthread processorThreadAfterReset([&](std::stop_token st) {
        processor.processLoop(st);
    });

    QTest::qWait(200);
    processorThreadAfterReset.request_stop();
    processorThreadAfterReset.join();

    QVERIFY2(capture.frameCount.load() == framesBeforeReset,
             qPrintable(u"Reset should have discarded the history: had %1 frames, now %2"_s.arg(framesBeforeReset).arg(capture.frameCount.load())));
}

std::vector<float> TestFftProcessor::createSineWave(float frequency, int sampleRate, int numSamples)
{
    std::vector<float> wave(static_cast<size_t>(numSamples));
    const float amplitude = 0.5f;
    for (int i = 0; i < numSamples; ++i) {
        wave[static_cast<size_t>(i)] =
            amplitude * std::sin(2.0f * std::numbers::pi_v<float> * frequency * static_cast<float>(i) / static_cast<float>(sampleRate));
    }
    return wave;
}

std::vector<float> TestFftProcessor::createSilence(int numSamples)
{
    return std::vector<float>(static_cast<size_t>(numSamples), 0.0f);
}

void TestFftProcessor::testProcessLoopSineWave()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    constexpr float freq = 440.0f;
    auto sineWave = createSineWave(freq, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);

    const auto written1 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written1 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isFrequenciesEmpty(frame), "FFT frame should have frequency data");
    QVERIFY2(!isBarEmpty(frame), "FFT frame should have bar data");
    QVERIFY2(capture.frameCount.load() > 0, "Frame callback should have been invoked");

    float peakMag = -80.0f;
    int peakBin = 0;
    for (size_t i = 0; i < frame.frequencies().size(); ++i) {
        if (frame.frequencies()[i] > peakMag) {
            peakMag = frame.frequencies()[i];
            peakBin = static_cast<int>(i);
        }
    }

    QVERIFY2(peakMag > -60.0f, qPrintable(QString("Peak magnitude %1 dB too low for sine wave"_L1).arg(peakMag)));

    const float melMin = 2595.0f * std::log10(1.0f + FftProcessor::MIN_FREQ / 700.0f);
    const float melMax = 2595.0f * std::log10(1.0f + std::min(FftProcessor::MAX_FREQ, static_cast<float>(sampleRate) / 2.0f) / 700.0f);
    const float inputMel = 2595.0f * std::log10(1.0f + freq / 700.0f);
    const float t = (inputMel - melMin) / (melMax - melMin);
    const int expectedBin = static_cast<int>(t * FftProcessor::NUM_LOG_BINS);
    const int binTolerance = 20;

    QVERIFY2(std::abs(peakBin - expectedBin) <= binTolerance,
             qPrintable(u"440 Hz sine: peak at bin %1, expected ~%2 (tolerance +/- %3)"_s.arg(peakBin).arg(expectedBin).arg(binTolerance)));
}

void TestFftProcessor::testProcessLoopSilence()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto silence = createSilence(static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written = writeBlocks(pipe.producer(), silence, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();

    QVERIFY2(!isFrequenciesEmpty(frame), "Silence should still produce a frame with frequency data");

    float maxMag = -200.0f;
    for (float mag : frame.frequencies()) {
        if (mag > maxMag)
            maxMag = mag;
    }
    QVERIFY2(maxMag < -60.0f, qPrintable(QString("Silence peak magnitude %1 dB too high should be below -60 dB"_L1).arg(maxMag)));
}

void TestFftProcessor::testProcessLoopMultipleFrames()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sine1kHz = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);

    for (int frame = 0; frame < 3; ++frame) {
        const auto written = writeBlocks(pipe.producer(), sine1kHz, sampleRate, channels);
        QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");
    }

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QTRY_VERIFY_WITH_TIMEOUT(capture.frameCount.load(std::memory_order_relaxed) > 0, 5000);
    quiescencePolling(capture.frameCount);

    processorThread.request_stop();
    processorThread.join();

    const auto frames = capture.frames();
    QVERIFY2(frames.size() >= 2, qPrintable(QString("Expected at least 2 frames from 3x FFT_SIZE data, got %1"_L1).arg(frames.size())));

    for (const auto &frame : frames) {
        QVERIFY2(!isFrequenciesEmpty(frame), "Each frame should have frequency data");
        QVERIFY(frame.frequencies().size() == static_cast<size_t>(FftProcessor::NUM_LOG_BINS));
        QVERIFY2(!isBarEmpty(frame), "Each frame should have bar data");
        QVERIFY(frame.bars().size() == static_cast<size_t>(FftProcessor::NUM_BAR_BINS));
    }
}

void TestFftProcessor::testFrameCallbackInvoked()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto noise = createSineWave(2000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written = writeBlocks(pipe.producer(), noise, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    QVERIFY2(capture.frameCount.load() >= 1, qPrintable(u"Callback should be invoked at least once, got %1"_s.arg(capture.frameCount.load())));
}

void TestFftProcessor::testPeakHoldDecay()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;

    Pipe<PcmBlock> pipe(256);

    auto sine = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE) * 2);
    auto silence = createSilence(static_cast<int>(FftProcessor::FFT_SIZE) * 4);

    const auto writtenSine = writeBlocks(pipe.producer(), sine, sampleRate, channels);
    QVERIFY2(writtenSine > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    const int framesAfterSine = capture.frameCount.load(std::memory_order_relaxed);
    const auto writtenSilence = writeBlocks(pipe.producer(), silence, sampleRate, channels);
    QVERIFY2(writtenSilence > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");
    // The silence only yields further frames once enough samples have
    // accumulated, which needs a pipe wait cycle: wait for actual progress
    // rather than for the counter to stop moving.
    QTRY_VERIFY_WITH_TIMEOUT(capture.frameCount.load(std::memory_order_relaxed) > framesAfterSine, 5000);
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    std::vector<float> peakValues;
    for (const auto &frame : capture.frames()) {
        if (!isBarEmpty(frame)) {
            peakValues.push_back(*std::max_element(frame.bars().begin(), frame.bars().end()));
        }
    }

    QVERIFY2(peakValues.size() >= 2, qPrintable(u"Expected at least 2 frames for decay verification, got %1"_s.arg(peakValues.size())));

    float initialPeak = peakValues.front();
    QVERIFY2(initialPeak > -20.0f, qPrintable(u"Initial peak from sine wave should be significant, got %1 dB"_s.arg(initialPeak)));

    float finalPeak = peakValues.back();
    QVERIFY2(finalPeak < initialPeak, qPrintable(u"Peak should decay after silence: initial=%1 dB, final=%2 dB"_s.arg(initialPeak).arg(finalPeak)));

    bool sawStrictDecay = false;
    for (size_t i = 1; i < peakValues.size(); ++i) {
        if (peakValues[i] < peakValues[i - 1] - 0.1f) {
            sawStrictDecay = true;
            break;
        }
    }
    QVERIFY2(sawStrictDecay, "Expected at least one strict decay step (> 0.1 dB decrease) after silence");
}

void TestFftProcessor::testBarDataSizeValidation()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    constexpr float freq = 1000.0f;
    auto sineWave = createSineWave(freq, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written2 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written2 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();

    QVERIFY(frame.bars().size() == static_cast<size_t>(FftProcessor::NUM_BAR_BINS));

    for (const float &val : frame.bars()) {
        QVERIFY2(val >= -85.0f && val <= 10.0f, qPrintable(u"Bar value out of range: %1"_s.arg(val)));
    }
}

void TestFftProcessor::testFftProducesOutputAboveThreshold_data()
{
    QTest::addColumn<float>("frequency");
    QTest::addColumn<int>("sampleRate");

    QTest::newRow("440Hz_44100") << 440.0f << 44100;
    QTest::newRow("1kHz_44100") << 1000.0f << 44100;
    QTest::newRow("4kHz_44100") << 4000.0f << 44100;
    QTest::newRow("8kHz_44100") << 8000.0f << 44100;
    QTest::newRow("440Hz_48000") << 440.0f << 48000;
    QTest::newRow("1kHz_48000") << 1000.0f << 48000;
}

void TestFftProcessor::testFftProducesOutputAboveThreshold()
{
    QFETCH(float, frequency);
    QFETCH(int, sampleRate);

    constexpr int channels = 1;
    auto sineWave = createSineWave(frequency, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written3 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written3 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();

    float peakMag = -80.0f;
    for (const float &mag : frame.frequencies()) {
        if (mag > peakMag)
            peakMag = mag;
    }

    QVERIFY2(peakMag > -50.0f,
             qPrintable(u"Peak magnitude %1 dB too low for %2 Hz sine wave at %3 Hz sample rate"_s.arg(peakMag).arg(frequency).arg(sampleRate)));

    int binsAboveNoise = 0;
    for (const float &mag : frame.frequencies()) {
        if (mag > -60.0f)
            binsAboveNoise++;
    }

    QVERIFY2(binsAboveNoise <= 50, qPrintable(u"Too many bins above noise floor (%1), signal may not be a clean sine"_s.arg(binsAboveNoise)));
}

void TestFftProcessor::testFrameTimestamp()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written1 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written1 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    std::optional<std::chrono::microseconds> firstTimestamp;
    std::atomic<int> frameCount{0};

    processor.setFrameCallback([&](const DragonFftFrame &frame) {
        if (!firstTimestamp.has_value()) {
            firstTimestamp = frame.timestamp();
        }
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    auto processorThread = startProcessorThread(processor);

    QTRY_VERIFY_WITH_TIMEOUT(frameCount.load(std::memory_order_relaxed) > 0, 5000);
    quiescencePolling(frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    QVERIFY(firstTimestamp.has_value());
    QVERIFY(firstTimestamp->count() > 0);
}

void TestFftProcessor::testFftModeOff()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);

    const auto written2 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written2 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Off);

    std::atomic<int> callbackCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackCount.fetch_add(1);
    });

    auto processorThread = startProcessorThread(processor);

    QTest::qWait(300);

    processorThread.request_stop();
    processorThread.join();

    QVERIFY2(callbackCount.load() == 0, qPrintable(QString("Off mode should produce zero frames, got %1"_L1).arg(callbackCount.load())));
}

void TestFftProcessor::testFftModeBarsOnly()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written3 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written3 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::BarsOnly);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isBarEmpty(frame), "BarsOnly mode should produce barData");
    QVERIFY2(frame.bars().size() == static_cast<size_t>(FftProcessor::NUM_BAR_BINS),
             qPrintable(QString("barData should have %1 elements, got %2"_L1).arg(FftProcessor::NUM_BAR_BINS).arg(frame.bars().size())));
    QVERIFY2(isFrequenciesEmpty(frame), "BarsOnly mode should not produce frequenciesDb");
    QVERIFY2(capture.frameCount.load() > 0, "Callback should have been invoked");
}

void TestFftProcessor::testFftModeDetailedOnly()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written4 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written4 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::DetailedOnly);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isFrequenciesEmpty(frame), "DetailedOnly mode should produce frequenciesDb");
    QVERIFY2(frame.frequencies().size() == static_cast<size_t>(FftProcessor::NUM_LOG_BINS),
             qPrintable(QString("frequenciesDb should have %1 elements, got %2"_L1).arg(FftProcessor::NUM_LOG_BINS).arg(frame.frequencies().size())));
    QVERIFY2(isBarEmpty(frame), "DetailedOnly mode should not produce barData");
    QVERIFY2(capture.frameCount.load() > 0, "Callback should have been invoked");
}

void TestFftProcessor::testFftModeSwitch()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    const auto written5 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written5 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    int framesPhase1 = capture.frameCount.load();
    QVERIFY2(framesPhase1 >= 1, qPrintable(QString("Both mode should produce at least 1 frame, got %1"_L1).arg(framesPhase1)));

    processor.setFftMode(FftProcessor::FftMode::Off);
    int framesPhase2Start = capture.frameCount.load();

    const auto written6 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written6 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QTest::qWait(300);
    int framesPhase2End = capture.frameCount.load();
    QVERIFY2(framesPhase2End == framesPhase2Start,
             qPrintable(QString("Off mode should not produce new frames: started at %1, ended at %2"_L1).arg(framesPhase2Start).arg(framesPhase2End)));

    processor.setFftMode(FftProcessor::FftMode::BarsOnly);
    const auto written7 = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written7 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QTRY_VERIFY_WITH_TIMEOUT(capture.frameCount.load(std::memory_order_relaxed) > framesPhase2End, 5000);
    quiescencePolling(capture.frameCount, 5000);
    int framesPhase3 = capture.frameCount.load();
    QVERIFY2(framesPhase3 > framesPhase2End,
             qPrintable(QString("BarsOnly mode should resume producing frames: had %1, now %2"_L1).arg(framesPhase2End).arg(framesPhase3)));

    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isBarEmpty(frame), "Latest frame should have barData after switching to BarsOnly");
    QVERIFY2(isFrequenciesEmpty(frame), "Latest frame should not have frequenciesDb in BarsOnly mode");
}

void TestFftProcessor::testFrameCountForThreeSecondsStereo()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;
    constexpr float durationSeconds = 3.0f;
    constexpr int totalFloats = static_cast<int>(sampleRate * durationSeconds * channels); // 264600 interleaved stereo floats

    // Silence is fine we are counting emitted frames, not validating content.
    auto audio = createSilence(totalFloats);

    // Pipe must hold ceil(264600 / 1024) = 259 blocks. 512 gives comfortable headroom.
    Pipe<PcmBlock> pipe(512);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    // Dump the entire 3-second burst (matches what the SDL dummy driver does in CI).
    const auto written = writeBlocks(pipe.producer(), audio, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    // Wait for the processor to drain all blocks and emit all owed frames.
    // A fixed qWait(500) is flaky under CPU contention: the processor thread
    // may not drain all 259 blocks in time when prior tests have loaded the
    // scheduler. Polling for quiescence makes the test deterministic.
    QTRY_VERIFY_WITH_TIMEOUT(capture.frameCount.load(std::memory_order_relaxed) > 0, 5000);
    const int count = quiescencePolling(capture.frameCount);

    processorThread.request_stop();
    processorThread.join();

    // Total mono samples after downmix: 264600 / 2 = 132300.
    // First frame at FFT_SIZE (4096), then every hop = sampleRate/60 = 735 samples.
    // Expected = floor((132300 - 4096) / 735) + 1 = 175 frames.
    // 10 % tolerance covers any edge-case rounding in block boundaries.
    QVERIFY2(count >= 158, qPrintable(u"Too few frames (%1) for 3-second stereo burst expected ~175"_s.arg(count)));
    QVERIFY2(count <= 192, qPrintable(u"Too many frames (%1) possible burst-emission bug"_s.arg(count)));
}

void TestFftProcessor::testConfigurableRate()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;
    constexpr float durationSeconds = 3.0f;
    constexpr int totalFloats = static_cast<int>(sampleRate * durationSeconds * channels);

    auto audio = createSineWave(1000.0f, sampleRate, totalFloats);

    Pipe<PcmBlock> pipe(512);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftRate(30);
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    const auto written = writeBlocks(pipe.producer(), audio, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QTRY_VERIFY_WITH_TIMEOUT(capture.frameCount.load(std::memory_order_relaxed) > 0, 5000);
    quiescencePolling(capture.frameCount);

    processorThread.request_stop();
    processorThread.join();

    const int count = capture.frameCount.load(std::memory_order_relaxed);

    QVERIFY2(count >= 80, qPrintable(u"Too few frames (%1) for 3-second burst at 30 Hz expected ~88"_s.arg(count)));
    QVERIFY2(count <= 95, qPrintable(u"Too many frames (%1) for 3-second burst at 30 Hz"_s.arg(count)));
}

void TestFftProcessor::testFftResumesAfterModeToggle()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;

    Pipe<PcmBlock> pipe(512);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::BarsOnly);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    // Feed a large burst to build up a deep history simulates a long-running session.
    constexpr int totalFloats = sampleRate * 10 * channels;
    auto audio = createSilence(totalFloats);
    const auto written1 = writeBlocks(pipe.producer(), audio, sampleRate, channels);
    QVERIFY2(written1 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    // Wait for the processor to actually start emitting frames before measuring.
    // Under ASAN, thread startup + first FFT can exceed the quiescence stability window.
    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return capture.frameCount.load(std::memory_order_relaxed) > 0;
                 },
                 30000),
             "Should have produced at least one frame during initial playback");

    quiescencePolling(capture.frameCount, 30000);
    int framesPhase1 = capture.frameCount.load(std::memory_order_relaxed);
    QVERIFY2(framesPhase1 > 0, "Should have produced frames during initial playback");

    // Turn Off thread goes to sleep but history stays filled.
    processor.setFftMode(FftProcessor::FftMode::Off);
    capture.frameCount.store(0, std::memory_order_relaxed);
    QTest::qWait(100);

    // Turn back On with a small amount of NEW data.
    auto newAudio = createSilence(static_cast<int>(FftProcessor::FFT_SIZE));
    const auto written2 = writeBlocks(pipe.producer(), newAudio, sampleRate, channels);
    QVERIFY2(written2 > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");
    processor.setFftMode(FftProcessor::FftMode::BarsOnly);

    // Wait for the processor to drain and emit frames from the new data.
    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return capture.frameCount.load(std::memory_order_relaxed) > 0;
                 },
                 30000),
             "Should resume producing frames after re-enable");

    quiescencePolling(capture.frameCount, 30000);
    int framesPhase2 = capture.frameCount.load(std::memory_order_relaxed);

    processorThread.request_stop();
    processorThread.join();

    // The key invariant is that the thread does NOT hang and does emit SOME frames.
    // The actual reset is done at the FftPipeline level (see test_player_fft.cpp).
    QVERIFY2(framesPhase2 > 0, "Should resume producing frames after re-enable");
}

void TestFftProcessor::testFftStopTokenHonoredInInnerLoop()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;

    Pipe<PcmBlock> pipe(512);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::BarsOnly);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Pre-fill a massive amount of data so that when the processor starts,
    // m_historyTotalSamples is huge relative to m_lastFrameAtSample, causing
    // the inner while-loop to have a lot of work to do.
    constexpr int totalFloats = sampleRate * 30 * channels;
    auto audio = createSilence(totalFloats);
    const auto written = writeBlocks(pipe.producer(), audio, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    auto processorThread = startProcessorThread(processor);

    // Wait just long enough for the processor to enter its inner loop.
    QTest::qWait(50);

    // Now request stop. Without the inner-loop stop-token check, the join()
    // would block indefinitely while the thread churns through the backlog.
    processorThread.request_stop();

    // The thread should exit promptly (well under 5 seconds) now that
    // the inner while-loop also checks st.stop_requested().
    auto start = std::chrono::steady_clock::now();
    processorThread.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    QVERIFY2(elapsed < std::chrono::seconds(5),
             qPrintable(u"Thread should stop quickly even with large backlog took %1 ms"_s.arg(
                 std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count())));
}

void TestFftProcessor::testFftFrequencyLocalization()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    constexpr float inputFreq = 1000.0f;

    auto sineWave = createSineWave(inputFreq, sampleRate, static_cast<int>(FftProcessor::FFT_SIZE));

    Pipe<PcmBlock> pipe(256);
    const auto written = writeBlocks(pipe.producer(), sineWave, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::DetailedOnly);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isFrequenciesEmpty(frame), "Should have frequency data");
    QVERIFY2(frame.frequencies().size() == static_cast<size_t>(FftProcessor::NUM_LOG_BINS),
             qPrintable(u"Expected %1 bins, got %2"_s.arg(FftProcessor::NUM_LOG_BINS).arg(frame.frequencies().size())));

    const float melMin = 2595.0f * std::log10(1.0f + FftProcessor::MIN_FREQ / 700.0f);
    const float melMax = 2595.0f * std::log10(1.0f + std::min(FftProcessor::MAX_FREQ, static_cast<float>(sampleRate) / 2.0f) / 700.0f);
    const float inputMel = 2595.0f * std::log10(1.0f + inputFreq / 700.0f);
    const float t = (inputMel - melMin) / (melMax - melMin);
    const int expectedBin = static_cast<int>(t * FftProcessor::NUM_LOG_BINS);

    int peakBin = 0;
    float peakMag = -200.0f;
    for (int i = 0; i < static_cast<int>(frame.frequencies().size()); ++i) {
        if (frame.frequencies()[static_cast<size_t>(i)] > peakMag) {
            peakMag = frame.frequencies()[static_cast<size_t>(i)];
            peakBin = i;
        }
    }

    QVERIFY2(peakMag > -50.0f, qPrintable(u"Peak magnitude %1 dB too low for %2 Hz sine"_s.arg(peakMag).arg(inputFreq)));

    const int binTolerance = 20;
    QVERIFY2(std::abs(peakBin - expectedBin) <= binTolerance,
             qPrintable(u"Peak at bin %1, expected ~%2 (tolerance +/-%3) for %4 Hz"_s.arg(peakBin).arg(expectedBin).arg(binTolerance).arg(inputFreq)));

    qDebug() << "Frequency localization: input" << inputFreq << "Hz -> peak at bin" << peakBin << "(expected" << expectedBin << ") magnitude" << peakMag
             << "dB";
}

void TestFftProcessor::testFftMultiTonePeaks()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 1;
    constexpr float freq1 = 440.0f;
    constexpr float freq2 = 5000.0f;

    constexpr int numSamples = static_cast<int>(FftProcessor::FFT_SIZE);
    std::vector<float> wave(static_cast<size_t>(numSamples));
    for (int i = 0; i < numSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        wave[static_cast<size_t>(i)] =
            0.25f * std::sin(2.0f * std::numbers::pi_v<float> * freq1 * t) + 0.25f * std::sin(2.0f * std::numbers::pi_v<float> * freq2 * t);
    }

    Pipe<PcmBlock> pipe(256);
    const auto written = writeBlocks(pipe.producer(), wave, sampleRate, channels);
    QVERIFY2(written > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::DetailedOnly);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    QVERIFY(waitForFirstFrame(capture.frameCount));
    quiescencePolling(capture.frameCount, 5000);
    processorThread.request_stop();
    processorThread.join();

    auto frame = capture.latest();
    QVERIFY2(!isFrequenciesEmpty(frame), "Should have frequency data");

    const float melMin = 2595.0f * std::log10(1.0f + FftProcessor::MIN_FREQ / 700.0f);
    const float melMax = 2595.0f * std::log10(1.0f + std::min(FftProcessor::MAX_FREQ, static_cast<float>(sampleRate) / 2.0f) / 700.0f);

    auto freqToBin = [&](float freq) {
        const float mel = 2595.0f * std::log10(1.0f + freq / 700.0f);
        const float t = (mel - melMin) / (melMax - melMin);
        return static_cast<int>(t * FftProcessor::NUM_LOG_BINS);
    };

    const int expectedBin1 = freqToBin(freq1);
    const int expectedBin2 = freqToBin(freq2);

    const int binTolerance = 20;

    auto findGlobalPeakExcluding = [&](int excludeCenter, int excludeRange) -> std::pair<int, float> {
        int bestBin = 0;
        float bestMag = -200.0f;
        int excludeLo = std::max(0, excludeCenter - excludeRange);
        int excludeHi = std::min(static_cast<int>(frame.frequencies().size()) - 1, excludeCenter + excludeRange);
        for (int i = 0; i < static_cast<int>(frame.frequencies().size()); ++i) {
            if (i >= excludeLo && i <= excludeHi)
                continue;
            if (frame.frequencies()[static_cast<size_t>(i)] > bestMag) {
                bestMag = frame.frequencies()[static_cast<size_t>(i)];
                bestBin = i;
            }
        }
        return {bestBin, bestMag};
    };

    auto [peak1Bin, peak1Mag] = findGlobalPeakExcluding(expectedBin2, binTolerance);
    auto [peak2Bin, peak2Mag] = findGlobalPeakExcluding(expectedBin1, binTolerance);

    QVERIFY2(peak1Mag > -50.0f, qPrintable(u"First tone (%1 Hz): peak %2 dB too low"_s.arg(freq1).arg(peak1Mag)));
    QVERIFY2(peak2Mag > -50.0f, qPrintable(u"Second tone (%1 Hz): peak %2 dB too low"_s.arg(freq2).arg(peak2Mag)));

    QVERIFY2(std::abs(peak1Bin - expectedBin1) <= binTolerance,
             qPrintable(u"First tone: global peak at bin %1, expected ~%2"_s.arg(peak1Bin).arg(expectedBin1)));
    QVERIFY2(std::abs(peak2Bin - expectedBin2) <= binTolerance,
             qPrintable(u"Second tone: global peak at bin %1, expected ~%2"_s.arg(peak2Bin).arg(expectedBin2)));

    QVERIFY2(std::abs(peak1Bin - peak2Bin) > binTolerance,
             qPrintable(u"Two tones should produce distinguishable peaks: peak1 at bin %1, peak2 at bin %2"_s.arg(peak1Bin).arg(peak2Bin)));

    qDebug() << "Multi-tone: " << freq1 << "Hz -> bin" << peak1Bin << "(expected" << expectedBin1 << ")" << freq2 << "Hz -> bin" << peak2Bin << "(expected"
             << expectedBin2 << ")";
}

void TestFftProcessor::testInBandFormatSwitchAtBlockBoundary()
{
    constexpr int firstRate = 44100;
    constexpr int firstChannels = 2;
    constexpr int secondRate = 48000;
    constexpr int secondChannels = 1;

    Pipe<PcmBlock> pipe(256);

    FftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setFftMode(FftProcessor::FftMode::Both);

    FrameCapture capture;
    processor.setFrameCallback([&capture](DragonFftFrame frame) {
        capture.capture(std::move(frame));
    });

    auto processorThread = startProcessorThread(processor);

    // Phase 1: interleaved stereo sine at 44100. The block metadata is the
    // only source of format information.
    auto stereo = createSineWave(440.0f, firstRate, static_cast<int>(FftProcessor::FFT_SIZE) * firstChannels * 2);
    const auto writtenFirst = writeBlocks(pipe.producer(), stereo, firstRate, firstChannels);
    QVERIFY2(writtenFirst > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return capture.frameCount.load(std::memory_order_relaxed) > 0;
                 },
                 5000),
             "Should have produced frames under the first format");
    quiescencePolling(capture.frameCount, 5000);
    const int framesFirstFormat = capture.frameCount.load(std::memory_order_relaxed);
    QVERIFY2(framesFirstFormat >= 2, qPrintable(u"Expected several frames under the first format, got %1"_s.arg(framesFirstFormat)));

    // Phase 2: the format switches to 48000/mono exactly at a block boundary.
    // The processor resets its format-dependent history and continues.
    auto mono = createSineWave(1000.0f, secondRate, static_cast<int>(FftProcessor::FFT_SIZE) * 2);
    const auto writtenSecond = writeBlocks(pipe.producer(), mono, secondRate, secondChannels);
    QVERIFY2(writtenSecond > 0, "writeBlocks wrote zero blocks pipe may be full or data empty");

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return capture.frameCount.load(std::memory_order_relaxed) > framesFirstFormat;
                 },
                 5000),
             "Frames must keep flowing after an in-band format switch");
    quiescencePolling(capture.frameCount, 5000);

    processorThread.request_stop();
    processorThread.join();

    const int framesAfterSwitch = capture.frameCount.load(std::memory_order_relaxed);
    QVERIFY2(framesAfterSwitch > framesFirstFormat,
             qPrintable(u"Format switch must not stall the pipeline: had %1 frames before, %2 after"_s.arg(framesFirstFormat).arg(framesAfterSwitch)));

    const auto frame = capture.latest();
    QVERIFY2(!isFrequenciesEmpty(frame), "Frame after the format switch should have frequency data");
    QVERIFY2(!isBarEmpty(frame), "Frame after the format switch should have bar data");
    QVERIFY(frame.frequencies().size() == static_cast<size_t>(FftProcessor::NUM_LOG_BINS));
    QVERIFY(frame.bars().size() == static_cast<size_t>(FftProcessor::NUM_BAR_BINS));

    // Values computed right after a format switch must be sane: no NaN/inf from
    // overflowing or dividing by a stale format.
    QVERIFY2(allFinite(frame.frequencies()), "Detailed values after the format switch should be finite");
    QVERIFY2(allFinite(frame.bars()), "Bar values after the format switch should be finite");
    for (const float &val : frame.bars()) {
        QVERIFY2(val >= -85.0f && val <= 10.0f, qPrintable(u"Bar value out of range after format switch: %1"_s.arg(val)));
    }
}

QTEST_MAIN(TestFftProcessor)
#include "test_fftprocessor.moc"
