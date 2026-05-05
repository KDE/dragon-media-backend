/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "../include/dragonsdl/dragonfftframe.h"
#include "../include/dragonsdl/dragonplayer.h"
#include "dragonsdl_export.h"
#include <stdfloat>

#include <array>
#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

template<typename T>
class LockFreeSpscQueue;

template<typename T>
class kissfft;

class DRAGONSDL_EXPORT DragonFftProcessor
{
public:
    static constexpr size_t FFT_SIZE = 4096;
    static constexpr float MIN_FREQ = 40.0f;
    static constexpr float MAX_FREQ = 16000.0f;
    static constexpr int NUM_LOG_BINS = 512;
    static constexpr int NUM_BAR_BINS = 24;

    using FrameCallback = std::move_only_function<void(DragonFftFrame)>;

    DragonFftProcessor();
    ~DragonFftProcessor();

    DragonFftProcessor(const DragonFftProcessor &) = delete;
    DragonFftProcessor &operator=(const DragonFftProcessor &) = delete;
    DragonFftProcessor(DragonFftProcessor &&) = delete;
    DragonFftProcessor &operator=(DragonFftProcessor &&) = delete;

    void setQueue(LockFreeSpscQueue<std::float32_t> *queue);
    void setWaitCv(std::condition_variable *cv);
    void setSampleRate(int sampleRate);

    using FftMode = DragonPlayer::FftMode;

    void setFftMode(FftMode mode);

    void setFrameCallback(FrameCallback cb);

    void processLoop(std::stop_token st);

    [[nodiscard]] DragonFftFrame takeLatestFrame();

    void reset();

    static void applyHannWindow(std::span<std::float32_t> data);

private:
    LockFreeSpscQueue<std::float32_t> *m_fftQueue = nullptr;
    std::condition_variable *m_waitCv = nullptr;
    std::mutex m_waitMutex;
    int m_sampleRate = 44100;
    std::atomic<FftMode> m_fftMode{FftMode::Off};

    std::unique_ptr<kissfft<float>> m_fft;
    std::array<std::float32_t, FFT_SIZE> m_inputWindow;
    std::array<std::float32_t, NUM_BAR_BINS> m_prevBarFrequencies;

    void transformReal(std::span<const std::float32_t, FFT_SIZE> input, std::span<std::complex<float>, FFT_SIZE / 2> output);

    FrameCallback m_frameCallback;

    std::mutex m_frameMutex;
    DragonFftFrame m_latestFrame;
};