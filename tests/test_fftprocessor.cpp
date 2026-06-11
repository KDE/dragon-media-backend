/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

using namespace Qt::StringLiterals;

#include "dragonfftprocessor.h"
#include "dragonpipe.h"
#include "dragonpipe_test_utils.h"
#include <DragonMultimedia/dragonfftframe.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>
#include <stop_token>
#include <thread>
#include <vector>

class TestFftProcessor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testHannWindow_data();
    void testHannWindow();

    void testConstruction();
    void testSetQueue();
    void testSetSampleRate();
    void testReset();
    void testTakeLatestFrameEmpty();

    void testProcessLoopSineWave();
    void testProcessLoopSilence();
    void testProcessLoopMultipleFrames();
    void testFrameCallbackInvoked();

    void testPeakHoldDecay();
    void testBarDataSizeValidation();
    void testFrequencyDetectionAccuracy_data();
    void testFrequencyDetectionAccuracy();
    void testFrameTimestamp();
    void testSampleRateChange();

    void testFftModeOff();
    void testFftModeBarsOnly();
    void testFftModeDetailedOnly();
    void testFftModeSwitch();
    void testFrameCountForThreeSecondsStereo();
    void testConfigurableRate();

    void testFftHistoryResetOnModeToggle();
    void testFftStopTokenHonoredInInnerLoop();

private:
    std::vector<std::float32_t> createSineWave(float frequency, int sampleRate, int numSamples);
    std::vector<std::float32_t> createSilence(int numSamples);
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

    std::vector<std::float32_t> data(static_cast<size_t>(windowSize), 1.0f);
    DragonFftProcessor::applyHannWindow(data);

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
    DragonFftProcessor processor;
    QVERIFY(true);
}

void TestFftProcessor::testSetQueue()
{
    DragonPipe<DragonFftBlock> pipe(256);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    QVERIFY(true);
}

void TestFftProcessor::testSetSampleRate()
{
    DragonFftProcessor processor;

    processor.setSampleRate(48000);
    QVERIFY(true);

    processor.setSampleRate(44100);
    QVERIFY(true);
}

void TestFftProcessor::testReset()
{
    DragonFftProcessor processor;
    processor.reset();
    QVERIFY(true);
}

void TestFftProcessor::testTakeLatestFrameEmpty()
{
    DragonFftProcessor processor;
    processor.setSampleRate(44100);

    auto frame = processor.takeLatestFrame();
    QVERIFY(frame.frequenciesDb.empty());
    QVERIFY(frame.barData.empty());
}

std::vector<std::float32_t> TestFftProcessor::createSineWave(float frequency, int sampleRate, int numSamples)
{
    std::vector<std::float32_t> wave(static_cast<size_t>(numSamples));
    const float amplitude = 0.5f;
    for (int i = 0; i < numSamples; ++i) {
        wave[static_cast<size_t>(i)] =
            amplitude * std::sin(2.0f * std::numbers::pi_v<float> * frequency * static_cast<float>(i) / static_cast<float>(sampleRate));
    }
    return wave;
}

std::vector<std::float32_t> TestFftProcessor::createSilence(int numSamples)
{
    return std::vector<std::float32_t>(static_cast<size_t>(numSamples), 0.0f);
}

void TestFftProcessor::testProcessLoopSineWave()
{
    constexpr int sampleRate = 44100;
    constexpr float freq = 440.0f;
    auto sineWave = createSineWave(freq, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);

    [[maybe_unused]] const auto written1 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::atomic<bool> callbackInvoked{false};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackInvoked = true;
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);

    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();
    QVERIFY2(!frame.frequenciesDb.empty(), "FFT frame should have frequency data");
    QVERIFY2(!frame.barData.empty(), "FFT frame should have bar data");
    QVERIFY2(callbackInvoked.load(), "Frame callback should have been invoked");

    float peakMag = -80.0f;
    for (size_t i = 0; i < frame.frequenciesDb.size(); ++i) {
        if (frame.frequenciesDb[i] > peakMag) {
            peakMag = frame.frequenciesDb[i];
        }
    }

    QVERIFY2(peakMag > -60.0f, qPrintable(QString("Peak magnitude %1 dB too low for sine wave"_L1).arg(peakMag)));
}

void TestFftProcessor::testProcessLoopSilence()
{
    constexpr int sampleRate = 44100;
    auto silence = createSilence(static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), silence);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);

    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();

    if (!frame.frequenciesDb.empty()) {
        float sum = 0.0f;
        for (float mag : frame.frequenciesDb) {
            sum += mag;
        }
        float avg = sum / static_cast<float>(frame.frequenciesDb.size());
        QVERIFY2(avg < -40.0f, qPrintable(QString("Silence average magnitude %1 dB too high"_L1).arg(avg)));
    }
}

void TestFftProcessor::testProcessLoopMultipleFrames()
{
    constexpr int sampleRate = 44100;
    auto sine1kHz = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);

    for (int frame = 0; frame < 3; ++frame) {
        [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), sine1kHz);
    }

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::vector<DragonFftFrame> frames;
    std::mutex framesMutex;

    processor.setFrameCallback([&](DragonFftFrame frame) {
        std::scoped_lock lock(framesMutex);
        frames.push_back(std::move(frame));
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(500);

    stopSource.request_stop();
    processorThread.join();

    std::scoped_lock lock(framesMutex);
    QVERIFY2(frames.size() >= 1, qPrintable(QString("Expected at least 1 frame, got %1"_L1).arg(frames.size())));

    for (const auto &frame : frames) {
        QVERIFY2(!frame.frequenciesDb.empty(), "Each frame should have frequency data");
        QVERIFY(frame.frequenciesDb.size() == static_cast<size_t>(DragonFftProcessor::NUM_LOG_BINS));
        QVERIFY2(!frame.barData.empty(), "Each frame should have bar data");
        QVERIFY(frame.barData.size() == static_cast<size_t>(DragonFftProcessor::NUM_BAR_BINS));
    }
}

void TestFftProcessor::testFrameCallbackInvoked()
{
    constexpr int sampleRate = 44100;
    auto noise = createSineWave(2000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), noise);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::atomic<int> callbackCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(200);

    stopSource.request_stop();
    processorThread.join();

    QVERIFY2(callbackCount.load() >= 1, qPrintable(u"Callback should be invoked at least once, got %1"_s.arg(callbackCount.load())));
}

void TestFftProcessor::testPeakHoldDecay()
{
    constexpr int sampleRate = 44100;
    auto silence = createSilence(static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), silence);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::vector<std::float32_t> peakValues;
    std::mutex mutex;

    processor.setFrameCallback([&](const DragonFftFrame &frame) {
        std::scoped_lock lock(mutex);
        if (!frame.barData.empty()) {
            float maxVal = *std::max_element(frame.barData.begin(), frame.barData.end());
            peakValues.push_back(maxVal);
        }
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(300);

    stopSource.request_stop();
    processorThread.join();

    std::scoped_lock lock(mutex);
    if (peakValues.size() >= 2) {
        for (size_t i = 1; i < peakValues.size(); ++i) {
            QVERIFY2(peakValues[i] <= peakValues[i - 1] + 2.0f,
                     qPrintable(u"Peak decay not working: frame %1 = %2, frame %3 = %4"_s.arg(i - 1).arg(peakValues[i - 1]).arg(i).arg(peakValues[i])));
        }
    }
}

void TestFftProcessor::testBarDataSizeValidation()
{
    constexpr int sampleRate = 44100;
    constexpr float freq = 1000.0f;
    auto sineWave = createSineWave(freq, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written2 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);
    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();

    QVERIFY(frame.barData.size() == static_cast<size_t>(DragonFftProcessor::NUM_BAR_BINS));

    for (const float &val : frame.barData) {
        QVERIFY2(val >= -85.0f && val <= 10.0f, qPrintable(u"Bar value out of range: %1"_s.arg(val)));
    }
}

void TestFftProcessor::testFrequencyDetectionAccuracy_data()
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

void TestFftProcessor::testFrequencyDetectionAccuracy()
{
    QFETCH(float, frequency);
    QFETCH(int, sampleRate);

    auto sineWave = createSineWave(frequency, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written3 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);
    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();

    float peakMag = -80.0f;
    for (const float &mag : frame.frequenciesDb) {
        if (mag > peakMag)
            peakMag = mag;
    }

    QVERIFY2(peakMag > -50.0f,
             qPrintable(u"Peak magnitude %1 dB too low for %2 Hz sine wave at %3 Hz sample rate"_s.arg(peakMag).arg(frequency).arg(sampleRate)));

    int binsAboveNoise = 0;
    for (const float &mag : frame.frequenciesDb) {
        if (mag > -60.0f)
            binsAboveNoise++;
    }

    QVERIFY2(binsAboveNoise <= 50, qPrintable(u"Too many bins above noise floor (%1), signal may not be a clean sine"_s.arg(binsAboveNoise)));
}

void TestFftProcessor::testFrameTimestamp()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written1 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::optional<std::chrono::microseconds> firstTimestamp;

    processor.setFrameCallback([&](const DragonFftFrame &frame) {
        if (!firstTimestamp.has_value()) {
            firstTimestamp = frame.timestamp;
        }
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);
    stopSource.request_stop();
    processorThread.join();

    QVERIFY(firstTimestamp.has_value());
    QVERIFY(firstTimestamp->count() > 0);
}

void TestFftProcessor::testSampleRateChange()
{
    DragonPipe<DragonFftBlock> pipe(256);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);

    processor.setSampleRate(44100);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    auto sine44k = createSineWave(1000.0f, 44100, static_cast<int>(DragonFftProcessor::FFT_SIZE));
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), sine44k);

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);

    processor.setSampleRate(48000);

    QTest::qWait(100);
    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();
    QVERIFY2(!frame.frequenciesDb.empty(), "Frame should be produced after sample rate change");
}

void TestFftProcessor::testFftModeOff()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);

    [[maybe_unused]] const auto written2 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Off);

    std::atomic<int> callbackCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(300);

    stopSource.request_stop();
    processorThread.join();

    QVERIFY2(callbackCount.load() == 0, qPrintable(QString("Off mode should produce zero frames, got %1"_L1).arg(callbackCount.load())));
}

void TestFftProcessor::testFftModeBarsOnly()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written3 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);

    std::atomic<bool> callbackInvoked{false};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackInvoked.store(true);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);

    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();
    QVERIFY2(!frame.barData.empty(), "BarsOnly mode should produce barData");
    QVERIFY2(frame.barData.size() == static_cast<size_t>(DragonFftProcessor::NUM_BAR_BINS),
             qPrintable(QString("barData should have %1 elements, got %2"_L1).arg(DragonFftProcessor::NUM_BAR_BINS).arg(frame.barData.size())));
    QVERIFY2(frame.frequenciesDb.empty(), "BarsOnly mode should not produce frequenciesDb");
    QVERIFY2(callbackInvoked.load(), "Callback should have been invoked");
}

void TestFftProcessor::testFftModeDetailedOnly()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);
    [[maybe_unused]] const auto written4 = writeBlocks(pipe.producer(), sineWave);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::DetailedOnly);

    std::atomic<bool> callbackInvoked{false};
    processor.setFrameCallback([&](DragonFftFrame) {
        callbackInvoked.store(true);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(100);

    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();
    QVERIFY2(!frame.frequenciesDb.empty(), "DetailedOnly mode should produce frequenciesDb");
    QVERIFY2(frame.frequenciesDb.size() == static_cast<size_t>(DragonFftProcessor::NUM_LOG_BINS),
             qPrintable(QString("frequenciesDb should have %1 elements, got %2"_L1).arg(DragonFftProcessor::NUM_LOG_BINS).arg(frame.frequenciesDb.size())));
    QVERIFY2(frame.barData.empty(), "DetailedOnly mode should not produce barData");
    QVERIFY2(callbackInvoked.load(), "Callback should have been invoked");
}

void TestFftProcessor::testFftModeSwitch()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    DragonPipe<DragonFftBlock> pipe(256);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(1);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    [[maybe_unused]] const auto written5 = writeBlocks(pipe.producer(), sineWave);

    QTest::qWait(100);
    int framesPhase1 = frameCount.load();
    QVERIFY2(framesPhase1 >= 1, qPrintable(QString("Both mode should produce at least 1 frame, got %1"_L1).arg(framesPhase1)));

    processor.setFftMode(DragonFftProcessor::FftMode::Off);
    int framesPhase2Start = frameCount.load();

    [[maybe_unused]] const auto written6 = writeBlocks(pipe.producer(), sineWave);

    QTest::qWait(300);
    int framesPhase2End = frameCount.load();
    QVERIFY2(framesPhase2End == framesPhase2Start,
             qPrintable(QString("Off mode should not produce new frames: started at %1, ended at %2"_L1).arg(framesPhase2Start).arg(framesPhase2End)));

    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);
    [[maybe_unused]] const auto written7 = writeBlocks(pipe.producer(), sineWave);

    QTest::qWait(200);
    int framesPhase3 = frameCount.load();
    QVERIFY2(framesPhase3 > framesPhase2End,
             qPrintable(QString("BarsOnly mode should resume producing frames: had %1, now %2"_L1).arg(framesPhase2End).arg(framesPhase3)));

    stopSource.request_stop();
    processorThread.join();

    auto frame = processor.takeLatestFrame();
    QVERIFY2(!frame.barData.empty(), "Latest frame should have barData after switching to BarsOnly");
    QVERIFY2(frame.frequenciesDb.empty(), "Latest frame should not have frequenciesDb in BarsOnly mode");
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
    DragonPipe<DragonFftBlock> pipe(512);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(channels);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    // Dump the entire 3-second burst (matches what the SDL dummy driver does in CI).
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), audio);

    // Give the processor thread time to drain the pipe and emit all owed frames.
    QTest::qWait(500);

    stopSource.request_stop();
    processorThread.join();

    const int count = frameCount.load(std::memory_order_relaxed);

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

    DragonPipe<DragonFftBlock> pipe(512);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(channels);
    processor.setSampleRate(sampleRate);
    processor.setFftRate(30);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), audio);

    QTest::qWait(500);

    stopSource.request_stop();
    processorThread.join();

    const int count = frameCount.load(std::memory_order_relaxed);

    QVERIFY2(count >= 80, qPrintable(u"Too few frames (%1) for 3-second burst at 30 Hz expected ~88"_s.arg(count)));
    QVERIFY2(count <= 95, qPrintable(u"Too many frames (%1) for 3-second burst at 30 Hz"_s.arg(count)));
}

void TestFftProcessor::testFftHistoryResetOnModeToggle()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;

    DragonPipe<DragonFftBlock> pipe(512);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(channels);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    // Feed a large burst to build up a deep history simulates a long-running session.
    constexpr int totalFloats = sampleRate * 10 * channels;
    auto audio = createSilence(totalFloats);
    [[maybe_unused]] const auto written1 = writeBlocks(pipe.producer(), audio);

    QTest::qWait(300);
    int framesPhase1 = frameCount.load(std::memory_order_relaxed);
    QVERIFY2(framesPhase1 > 0, "Should have produced frames during initial playback");

    // Turn Off thread goes to sleep but history stays filled.
    processor.setFftMode(DragonFftProcessor::FftMode::Off);
    frameCount.store(0, std::memory_order_relaxed);
    QTest::qWait(100);

    // Turn back On with a small amount of NEW data.
    auto newAudio = createSilence(static_cast<int>(DragonFftProcessor::FFT_SIZE));
    [[maybe_unused]] const auto written2 = writeBlocks(pipe.producer(), newAudio);
    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);

    // Give time to drain and emit frames from new data only.
    QTest::qWait(500);
    int framesPhase2 = frameCount.load(std::memory_order_relaxed);

    stopSource.request_stop();
    processorThread.join();

    // The key invariant is that the thread does NOT hang and does emit SOME frames.
    // The actual reset is done at the DragonFftPipeline level (see test_player_fft.cpp).
    QVERIFY2(framesPhase2 > 0, "Should resume producing frames after re-enable");
}

void TestFftProcessor::testFftStopTokenHonoredInInnerLoop()
{
    constexpr int sampleRate = 44100;
    constexpr int channels = 2;

    DragonPipe<DragonFftBlock> pipe(512);

    DragonFftProcessor processor;
    processor.setConsumer(pipe.consumer());
    processor.setChannelCount(channels);
    processor.setSampleRate(sampleRate);
    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);

    std::atomic<int> frameCount{0};
    processor.setFrameCallback([&](DragonFftFrame) {
        frameCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Pre-fill a massive amount of data so that when the processor starts,
    // m_historyTotalSamples is huge relative to m_lastFrameAtSample, causing
    // the inner while-loop to have a lot of work to do.
    constexpr int totalFloats = sampleRate * 30 * channels;
    auto audio = createSilence(totalFloats);
    [[maybe_unused]] const auto written = writeBlocks(pipe.producer(), audio);

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    // Wait just long enough for the processor to enter its inner loop.
    QTest::qWait(50);

    // Now request stop. Without the inner-loop stop-token check, the join()
    // would block indefinitely while the thread churns through the backlog.
    stopSource.request_stop();

    // The thread should exit promptly (well under 5 seconds) now that
    // the inner while-loop also checks st.stop_requested().
    auto start = std::chrono::steady_clock::now();
    processorThread.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    QVERIFY2(elapsed < std::chrono::seconds(5),
             qPrintable(u"Thread should stop quickly even with large backlog took %1 ms"_s.arg(
                 std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count())));
}

QTEST_MAIN(TestFftProcessor)
#include "test_fftprocessor.moc"