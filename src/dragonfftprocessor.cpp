/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonfftprocessor.h>
#include <stdfloat>

#include <kissfft.hh>

#include "dragonsdl_fft_logging.h"

#include "dragonpipe.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>
#include <ranges>
#include <thread>

namespace
{
constexpr float hzToMel(float f)
{
    return 2595.0f * std::log10(1.0f + f / 700.0f);
}

constexpr float melToHz(float m)
{
    return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f);
}
}

using namespace std::chrono_literals;

DragonFftProcessor::DragonFftProcessor()
    : m_fft(std::make_unique<kissfft<float>>(FFT_SIZE / 2, false))
    , m_inputWindow{}
    , m_prevBarFrequencies{}
{
    m_prevBarFrequencies.fill(-80.0f);
}

DragonFftProcessor::~DragonFftProcessor() = default;

void DragonFftProcessor::setConsumer(DragonPipe<std::float32_t>::Consumer consumer)
{
    m_consumer = consumer;
}

void DragonFftProcessor::setChannelCount(int channels)
{
    m_channelCount = std::max(1, channels);
}

void DragonFftProcessor::setSampleRate(int sampleRate)
{
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
}

void DragonFftProcessor::setFftMode(FftMode mode)
{
    m_fftMode.store(mode, std::memory_order_relaxed);
}

void DragonFftProcessor::reset()
{
    m_prevBarFrequencies.fill(-80.0f);
}

void DragonFftProcessor::transformReal(std::span<const std::float32_t, FFT_SIZE> input, std::span<std::complex<float>, FFT_SIZE / 2> output)
{
    m_fft->transform_real(reinterpret_cast<const float *>(input.data()), output.data());
}

void DragonFftProcessor::setFrameCallback(FrameCallback cb)
{
    m_frameCallback = std::move(cb);
}

DragonFftFrame DragonFftProcessor::takeLatestFrame()
{
    std::scoped_lock lock(m_frameMutex);
    DragonFftFrame result = std::move(m_latestFrame);
    m_latestFrame = {};
    return result;
}

void DragonFftProcessor::processLoop(std::stop_token st)
{
    int frameCount = 0;
    auto prevMode = FftMode::Both;

    while (!st.stop_requested()) {
        const FftMode mode = m_fftMode.load(std::memory_order_relaxed);

        if (mode == FftMode::Off) {
            prevMode = FftMode::Off;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (prevMode == FftMode::Off && (mode == FftMode::BarsOnly || mode == FftMode::Both)) {
            m_prevBarFrequencies.fill(-80.0f);
        }
        prevMode = mode;

        if (!tryReadAndDownmix(st)) {
            if (st.stop_requested())
                break;
            continue;
        }

        applyHannWindow(m_inputWindow);

        std::array<std::complex<float>, FFT_SIZE / 2> fftOut;
        transformReal(m_inputWindow, fftOut);

        const float binToFreq = static_cast<float>(m_sampleRate.load(std::memory_order_relaxed)) / static_cast<float>(FFT_SIZE);
        DragonFftFrame frame;

        if (mode == FftMode::DetailedOnly || mode == FftMode::Both) {
            fillDetailedBins(frame, fftOut, binToFreq);
        }
        if (mode == FftMode::BarsOnly || mode == FftMode::Both) {
            fillBarBins(frame, fftOut, binToFreq);
        }
        frame.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());

        emitFrame(frame, ++frameCount, mode);
    }
}

bool DragonFftProcessor::tryReadAndDownmix(std::stop_token st)
{
    const size_t samplesNeeded = FFT_SIZE * static_cast<size_t>(m_channelCount);

    if (!m_consumer.waitFor(samplesNeeded, st)) {
        return false;
    }

    if (m_fftMode.load(std::memory_order_relaxed) == FftMode::Off) {
        return false;
    }

    m_consumer.readSomeWith(samplesNeeded, [this](std::span<const std::float32_t> b1, std::span<const std::float32_t> b2) {
        size_t outIdx = 0;
        const int ch = m_channelCount;

        auto downmixBlock = [&](std::span<const std::float32_t> block) {
            for (size_t i = 0; i + static_cast<size_t>(ch) <= block.size() && outIdx < FFT_SIZE; i += static_cast<size_t>(ch), ++outIdx) {
                std::float32_t sum = 0;
                for (int c = 0; c < ch; ++c) {
                    sum += block[i + static_cast<size_t>(c)];
                }
                m_inputWindow[outIdx] = sum / static_cast<std::float32_t>(ch);
            }
        };

        downmixBlock(b1);
        downmixBlock(b2);
    });

    return true;
}

float DragonFftProcessor::getMagnitude(std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, const float binToFreq, const int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(fftOut.size()))
        return 0.0f;

    float mag;
    if (idx == 0) {
        mag = std::abs(fftOut[0].real()) / static_cast<float>(FFT_SIZE);
    } else {
        mag = std::abs(fftOut[static_cast<size_t>(idx)]) / static_cast<float>(FFT_SIZE);
    }

    const float freq = static_cast<float>(idx) * binToFreq;
    const float tilt = std::sqrt(std::max(freq, MIN_FREQ) / MIN_FREQ);
    return mag * tilt;
}

float DragonFftProcessor::computeMelBin(std::span<const std::complex<float>, FFT_SIZE / 2> fftOut,
                                        float binToFreq,
                                        float melMin,
                                        float melMax,
                                        float t0,
                                        float t1) const
{
    const float freq0 = melToHz(melMin + t0 * (melMax - melMin));
    const float freq1 = melToHz(melMin + t1 * (melMax - melMin));

    const float binIdx0 = freq0 / binToFreq;
    const float binIdx1 = freq1 / binToFreq;

    const int startBin = static_cast<int>(std::floor(binIdx0));
    const int endBin = static_cast<int>(std::ceil(binIdx1));

    float maxMag = 0.0f;
    if (endBin <= startBin + 1) {
        const float frac = binIdx0 - static_cast<float>(startBin);
        maxMag = std::lerp(getMagnitude(fftOut, binToFreq, startBin), getMagnitude(fftOut, binToFreq, startBin + 1), frac);
    } else {
        const int clampedEnd = std::min(endBin, static_cast<int>(fftOut.size()));
        auto binRange = std::views::iota(startBin, clampedEnd);
        if (!std::ranges::empty(binRange)) {
            maxMag = std::ranges::max(binRange | std::views::transform([&](int i) {
                                          return getMagnitude(fftOut, binToFreq, i);
                                      }));
        }
    }

    return 20.0f * std::log10(std::max(maxMag, 1e-6f));
}

void DragonFftProcessor::fillDetailedBins(DragonFftFrame &frame, std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq) const
{
    const float melMin = hzToMel(MIN_FREQ);
    const float melMax = hzToMel(std::min(MAX_FREQ, static_cast<float>(m_sampleRate.load(std::memory_order_relaxed)) / 2.0f));

    std::array<std::float32_t, NUM_LOG_BINS> logBins{};
    for (auto [i, bin] : std::views::enumerate(logBins)) {
        const float t0 = static_cast<float>(i) / static_cast<float>(NUM_LOG_BINS);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(NUM_LOG_BINS);
        bin = computeMelBin(fftOut, binToFreq, melMin, melMax, t0, t1);
    }
    frame.frequenciesDb.assign_range(logBins);
}

void DragonFftProcessor::fillBarBins(DragonFftFrame &frame, std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq)
{
    const float melMin = hzToMel(MIN_FREQ);
    const float melMax = hzToMel(std::min(MAX_FREQ, static_cast<float>(m_sampleRate.load(std::memory_order_relaxed)) / 2.0f));

    std::array<std::float32_t, NUM_BAR_BINS> barBins{};
    for (auto [i, bin] : std::views::enumerate(barBins)) {
        const float t0 = static_cast<float>(i) / static_cast<float>(NUM_BAR_BINS);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(NUM_BAR_BINS);
        bin = computeMelBin(fftOut, binToFreq, melMin, melMax, t0, t1);
    }

    constexpr float decayRate = 1.5f;
    for (auto [prev, curr] : std::views::zip(m_prevBarFrequencies, barBins)) {
        prev = std::max(curr, prev - decayRate);
        curr = prev;
    }

    frame.barData.assign_range(barBins);
}

void DragonFftProcessor::emitFrame(const DragonFftFrame &frame, int frameCount, FftMode mode)
{
    {
        std::scoped_lock lock(m_frameMutex);
        m_latestFrame = frame;
    }

    if (m_frameCallback)
        m_frameCallback(frame);

    if (frameCount <= 3 || frameCount % 60 == 0) {
        if (!frame.barData.empty()) {
            qCDebug(dragonsdlFft) << "frame emitted count=" << frameCount << "mode=" << mode << "barData[0]=" << frame.barData[0]
                                  << "barData[11]=" << frame.barData[11] << "barData[23]=" << frame.barData[23];
        } else if (!frame.frequenciesDb.empty()) {
            qCDebug(dragonsdlFft) << "frame emitted count=" << frameCount << "mode=" << mode << "freqDb[0]=" << frame.frequenciesDb[0]
                                  << "freqDb[256]=" << frame.frequenciesDb[256];
        } else {
            qCDebug(dragonsdlFft) << "frame emitted count=" << frameCount << "mode=" << mode;
        }
    }
}

void DragonFftProcessor::applyHannWindow(std::span<std::float32_t> data)
{
    const auto size = static_cast<float>(data.size());
    for (auto [i, val] : std::views::enumerate(data)) {
        const float window = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / (size - 1.0f)));
        val *= window;
    }
}