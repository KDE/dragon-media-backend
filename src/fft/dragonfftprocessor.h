/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "DragonMediaBackend/dragonfftframe.h"
#include "DragonMediaBackend/dragonspectrumanalyzer.h"
#include "dragonmediabackend_export.h"

#include <array>
#include <atomic>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>

#include "dragonpcmblock.h"
#include "player/dragonpipe.h"

template<typename T>
class kissfft;

class DRAGONMEDIABACKEND_EXPORT DragonFftProcessor
{
public:
    static constexpr size_t FFT_SIZE = 4096;
    static constexpr float MIN_FREQ = 40.0f;
    static constexpr float MAX_FREQ = 16000.0f;
    static constexpr int NUM_LOG_BINS = DragonFftFrame::NUM_FREQUENCIES;
    static constexpr int NUM_BAR_BINS = DragonFftFrame::NUM_BARS;

    using FrameCallback = std::move_only_function<void(DragonFftFrame)>;

    DragonFftProcessor();
    ~DragonFftProcessor();

    DragonFftProcessor(const DragonFftProcessor &) = delete;
    DragonFftProcessor &operator=(const DragonFftProcessor &) = delete;
    DragonFftProcessor(DragonFftProcessor &&) = delete;
    DragonFftProcessor &operator=(DragonFftProcessor &&) = delete;

    void setConsumer(DragonPipe<DragonPcmBlock>::Consumer consumer);

    void setChannelCount(int channels);

    void setSampleRate(int sampleRate);
    using FftMode = DragonSpectrumAnalyzer::Mode;

    void setFftMode(FftMode mode);
    void setFftRate(int rate);

    void setFrameCallback(FrameCallback cb);

    void processLoop(std::stop_token st);

    [[nodiscard]] DragonFftFrame takeLatestFrame();

    void reset();

    static void applyHannWindow(std::span<float> data);

private:
    DragonPipe<DragonPcmBlock>::Consumer m_consumer;
    int m_channelCount = 2;
    std::atomic<int> m_sampleRate{44100};
    std::atomic<FftMode> m_fftMode{FftMode::Off};
    std::atomic<int> m_fftRate{60};

    std::unique_ptr<kissfft<float>> m_fft;
    std::array<float, FFT_SIZE> m_inputWindow;
    std::array<float, NUM_BAR_BINS> m_prevBarFrequencies;
    void transformReal(std::span<const float, FFT_SIZE> input, std::span<std::complex<float>, FFT_SIZE / 2> output);

    bool drainPipeToHistory(std::stop_token st);
    void readWindowEndingAt(size_t endPos, std::span<float, FFT_SIZE> out);
    bool historyHasEnoughForWindow() const;

    [[nodiscard]] float getMagnitude(std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq, int idx) const;

    [[nodiscard]] float
    computeMelBin(std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq, float melMin, float melMax, float t0, float t1) const;

    void fillDetailedBins(std::span<float> frequenciesDb, std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq) const;

    void fillBarBins(std::span<float> barData, std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq, float decayRate);

    void emitFrame(const DragonFftFrame &frame);

    FrameCallback m_frameCallback;

    std::mutex m_frameMutex;
    DragonFftFrame m_latestFrame;

    static constexpr size_t HISTORY_SIZE = FFT_SIZE * 2;

    std::array<float, HISTORY_SIZE> m_sampleHistory{};
    size_t m_historyWritePos = 0;
    size_t m_historyTotalSamples = 0;
    size_t m_lastFrameAtSample = 0;

    std::chrono::microseconds m_newestBlockPts{};
};