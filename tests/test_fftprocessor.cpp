/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

using namespace Qt::StringLiterals;

#include "dragonfftprocessor.h"
#include <LockFreeSpscQueue.h>
#include <dragonsdl/dragonfftframe.h>

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
    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(silence.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < silence.size())
                v = silence[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < silence.size())
                v = silence[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    for (int frame = 0; frame < 3; ++frame) {
        [[maybe_unused]] const auto written = queue.try_write(sine1kHz.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
            size_t i = 0;
            for (std::float32_t &v : b1) {
                if (i < sine1kHz.size())
                    v = sine1kHz[i++];
            }
            for (std::float32_t &v : b2) {
                if (i < sine1kHz.size())
                    v = sine1kHz[i++];
            }
        });
    }

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(noise.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < noise.size())
                v = noise[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < noise.size())
                v = noise[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(silence.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < silence.size())
                v = silence[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < silence.size())
                v = silence[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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
    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    DragonFftProcessor processor;
    processor.setQueue(&queue);

    processor.setSampleRate(44100);
    processor.setFftMode(DragonFftProcessor::FftMode::Both);

    auto sine44k = createSineWave(1000.0f, 44100, static_cast<int>(DragonFftProcessor::FFT_SIZE));
    [[maybe_unused]] const auto written = queue.try_write(sine44k.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sine44k.size())
                v = sine44k[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sine44k.size())
                v = sine44k[i++];
        }
    });

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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    [[maybe_unused]] const auto written = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    DragonFftProcessor processor;
    processor.setQueue(&queue);
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

    [[maybe_unused]] const auto written1 = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    QTest::qWait(100);
    int framesPhase1 = frameCount.load();
    QVERIFY2(framesPhase1 >= 1, qPrintable(QString("Both mode should produce at least 1 frame, got %1"_L1).arg(framesPhase1)));

    processor.setFftMode(DragonFftProcessor::FftMode::Off);
    int framesPhase2Start = frameCount.load();

    [[maybe_unused]] const auto written2 = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

    QTest::qWait(300);
    int framesPhase2End = frameCount.load();
    QVERIFY2(framesPhase2End == framesPhase2Start,
             qPrintable(QString("Off mode should not produce new frames: started at %1, ended at %2"_L1).arg(framesPhase2Start).arg(framesPhase2End)));

    processor.setFftMode(DragonFftProcessor::FftMode::BarsOnly);
    [[maybe_unused]] const auto written3 = queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
        size_t i = 0;
        for (std::float32_t &v : b1) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
        for (std::float32_t &v : b2) {
            if (i < sineWave.size())
                v = sineWave[i++];
        }
    });

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

QTEST_MAIN(TestFftProcessor)
#include "test_fftprocessor.moc"