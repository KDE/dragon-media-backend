/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <QtTest>
#include <stdfloat>

#include <LockFreeSpscQueue.h>
#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonfftprocessor.h>

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

private slots:
    void testHannWindow_data();
    void testHannWindow();

    void testMelConversion_data();
    void testMelConversion();

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
    void testMelBinningRange();
    void testBarDataSizeValidation();
    void testFrequencyDetectionAccuracy_data();
    void testFrequencyDetectionAccuracy();
    void testFrameTimestamp();
    void testSampleRateChange();

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
                 qPrintable(QString("Window not symmetric at index %1: %2 vs %3")
                                .arg(i)
                                .arg(data[static_cast<size_t>(i)])
                                .arg(data[static_cast<size_t>(windowSize - 1 - i)])));
    }

    for (const float &val : data) {
        QVERIFY2(val >= 0.0f && val <= 1.0f, qPrintable(QString("Window value out of range: %1").arg(val)));
    }
}

void TestFftProcessor::testMelConversion_data()
{
    QTest::addColumn<float>("frequency");
    QTest::addColumn<float>("expectedMel");

    QTest::newRow("0Hz") << 0.0f << 0.0f;
    QTest::newRow("1kHz") << 1000.0f << 2595.0f * std::log10(1.0f + 1000.0f / 700.0f);
    QTest::newRow("4kHz") << 4000.0f << 2595.0f * std::log10(1.0f + 4000.0f / 700.0f);
    QTest::newRow("16kHz") << 16000.0f << 2595.0f * std::log10(1.0f + 16000.0f / 700.0f);
}

void TestFftProcessor::testMelConversion()
{
    QFETCH(float, frequency);
    QFETCH(float, expectedMel);

    float mel = DragonFftProcessor::hzToMel(frequency);
    float tolerance = std::max(1.0f, expectedMel * 0.01f);
    QVERIFY2(std::abs(mel - expectedMel) < tolerance, qPrintable(QString("hzToMel(%1) = %2, expected ~%3").arg(frequency).arg(mel).arg(expectedMel)));

    float hz = DragonFftProcessor::melToHz(mel);
    float hzTolerance = std::max(1.0f, frequency * 0.02f);
    if (frequency > 0.0f) {
        QVERIFY2(std::abs(hz - frequency) < hzTolerance, qPrintable(QString("melToHz(hzToMel(%1)) = %2, expected ~%1").arg(frequency).arg(hz)));
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

    queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

    int peakBin = 0;
    float peakMag = -80.0f;
    for (size_t i = 0; i < frame.frequenciesDb.size(); ++i) {
        if (frame.frequenciesDb[i] > peakMag) {
            peakMag = frame.frequenciesDb[i];
            peakBin = static_cast<int>(i);
        }
    }

    QVERIFY2(peakMag > -60.0f, qPrintable(QString("Peak magnitude %1 dB too low for sine wave").arg(peakMag)));
}

void TestFftProcessor::testProcessLoopSilence()
{
    constexpr int sampleRate = 44100;
    auto silence = createSilence(static_cast<int>(DragonFftProcessor::FFT_SIZE));

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    queue.try_write(silence.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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
        QVERIFY2(avg < -40.0f, qPrintable(QString("Silence average magnitude %1 dB too high").arg(avg)));
    }
}

void TestFftProcessor::testProcessLoopMultipleFrames()
{
    constexpr int sampleRate = 44100;
    auto sine1kHz = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};

    for (int frame = 0; frame < 3; ++frame) {
        queue.try_write(sine1kHz.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

    std::vector<DragonFftFrame> frames;
    std::mutex framesMutex;

    processor.setFrameCallback([&](DragonFftFrame frame) {
        std::lock_guard lock(framesMutex);
        frames.push_back(std::move(frame));
    });

    std::stop_source stopSource;
    std::jthread processorThread([&](std::stop_token) {
        processor.processLoop(stopSource.get_token());
    });

    QTest::qWait(500);

    stopSource.request_stop();
    processorThread.join();

    std::lock_guard lock(framesMutex);
    QVERIFY2(frames.size() >= 1, qPrintable(QString("Expected at least 1 frame, got %1").arg(frames.size())));

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
    queue.try_write(noise.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

    QVERIFY2(callbackCount.load() >= 1, qPrintable(QString("Callback should be invoked at least once, got %1").arg(callbackCount.load())));
}

void TestFftProcessor::testPeakHoldDecay()
{
    constexpr int sampleRate = 44100;
    auto silence = createSilence(static_cast<int>(DragonFftProcessor::FFT_SIZE));

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    queue.try_write(silence.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

    std::vector<std::float32_t> peakValues;
    std::mutex mutex;

    processor.setFrameCallback([&](const DragonFftFrame &frame) {
        std::lock_guard lock(mutex);
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

    std::lock_guard lock(mutex);
    if (peakValues.size() >= 2) {
        for (size_t i = 1; i < peakValues.size(); ++i) {
            QVERIFY2(peakValues[i] <= peakValues[i - 1] + 2.0f,
                     qPrintable(QString("Peak decay not working: frame %1 = %2, frame %3 = %4").arg(i - 1).arg(peakValues[i - 1]).arg(i).arg(peakValues[i])));
        }
    }
}

void TestFftProcessor::testMelBinningRange()
{
    constexpr int sampleRate = 44100;

    const float melMin = DragonFftProcessor::hzToMel(DragonFftProcessor::MIN_FREQ);
    const float melMax = DragonFftProcessor::hzToMel(DragonFftProcessor::MAX_FREQ);

    float minFreq = DragonFftProcessor::melToHz(melMin);
    float maxFreq = DragonFftProcessor::melToHz(melMax);

    QVERIFY2(minFreq >= 35.0f && minFreq <= 45.0f, qPrintable(QString("MIN_FREQ bin %1 Hz outside expected range [35, 45]").arg(minFreq)));
    QVERIFY2(maxFreq >= 15000.0f && maxFreq <= 17000.0f, qPrintable(QString("MAX_FREQ bin %1 Hz outside expected range [15000, 17000]").arg(maxFreq)));
}

void TestFftProcessor::testBarDataSizeValidation()
{
    constexpr int sampleRate = 44100;
    constexpr float freq = 1000.0f;
    auto sineWave = createSineWave(freq, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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
        QVERIFY2(val >= -85.0f && val <= 10.0f, qPrintable(QString("Bar value out of range: %1").arg(val)));
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
    queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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
             qPrintable(QString("Peak magnitude %1 dB too low for %2 Hz sine wave at %3 Hz sample rate").arg(peakMag).arg(frequency).arg(sampleRate)));

    int binsAboveNoise = 0;
    for (const float &mag : frame.frequenciesDb) {
        if (mag > -60.0f)
            binsAboveNoise++;
    }

    QVERIFY2(binsAboveNoise <= 50, qPrintable(QString("Too many bins above noise floor (%1), signal may not be a clean sine").arg(binsAboveNoise)));
}

void TestFftProcessor::testFrameTimestamp()
{
    constexpr int sampleRate = 44100;
    auto sineWave = createSineWave(1000.0f, sampleRate, static_cast<int>(DragonFftProcessor::FFT_SIZE));

    std::vector<std::float32_t> buffer(65536);
    LockFreeSpscQueue<std::float32_t> queue{std::span{buffer}};
    queue.try_write(sineWave.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

    auto sine44k = createSineWave(1000.0f, 44100, static_cast<int>(DragonFftProcessor::FFT_SIZE));
    queue.try_write(sine44k.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
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

QTEST_MAIN(TestFftProcessor)
#include "test_fftprocessor.moc"