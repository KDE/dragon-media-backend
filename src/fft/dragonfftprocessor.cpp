/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragonfftprocessor.h>
#include <stdfloat>

#include <kissfft.hh>

#include "dragonmultimedia_fft_logging.h"

#include "player/dragonpipe.h"
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

void DragonFftProcessor::setConsumer(DragonPipe<DragonFftBlock>::Consumer consumer)
{
    m_consumer = std::move(consumer);
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

void DragonFftProcessor::setFftRate(int rate)
{
    m_fftRate.store(rate, std::memory_order_relaxed);
}

void DragonFftProcessor::reset()
{
    m_prevBarFrequencies.fill(-80.0f);
    m_historyWritePos = 0;
    m_historyTotalSamples = 0;
    m_lastFrameAtSample = 0;
    m_newestBlockPts = {};
    m_sampleHistory.fill(0.0f);
    {
        std::scoped_lock lock(m_frameMutex);
        m_latestFrame = {};
    }
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
    auto prevMode = FftMode::Off;
    bool primed = false;

    while (!st.stop_requested()) {
        const FftMode mode = m_fftMode.load(std::memory_order_relaxed);

        if (mode == FftMode::Off) {
            prevMode = FftMode::Off;
            primed = false;
            drainPipeToHistory(st);
            m_consumer.waitFor(1, st);
            continue;
        }

        if (prevMode == FftMode::Off && (mode == FftMode::BarsOnly || mode == FftMode::Both)) {
            m_prevBarFrequencies.fill(-80.0f);
        }
        prevMode = mode;

        const int sr = m_sampleRate.load(std::memory_order_relaxed);
        const int actualRate = std::max(1, m_fftRate.load(std::memory_order_relaxed));
        const size_t hopSamples = static_cast<size_t>(sr) / static_cast<size_t>(actualRate);
        const bool blockMode = hopSamples >= FFT_SIZE;
        const size_t step = blockMode ? FFT_SIZE : hopSamples;
        const size_t prime = blockMode ? 0 : FFT_SIZE - hopSamples;
        const float effectiveRate = static_cast<float>(sr) / static_cast<float>(step);
        const float decayRate = 1.5f * (60.0f / effectiveRate);

        bool gotData = drainPipeToHistory(st);

        if (!primed) {
            if (historyHasEnoughForWindow()) {
                primed = true;
                m_lastFrameAtSample = prime;
            } else {
                if (!gotData && !st.stop_requested())
                    m_consumer.waitFor(1, st);
                continue;
            }
        }

        while (m_historyTotalSamples >= (m_lastFrameAtSample + step) && !st.stop_requested()) {
            readWindowEndingAt(m_lastFrameAtSample + step - 1, m_inputWindow);
            applyHannWindow(std::span<std::float32_t>(m_inputWindow));

            std::array<std::complex<float>, FFT_SIZE / 2> fftOut;
            transformReal(m_inputWindow, fftOut);

            const float binToFreq = static_cast<float>(sr) / static_cast<float>(FFT_SIZE);
            DragonFftFrame frame;

            if (mode == FftMode::DetailedOnly || mode == FftMode::Both) {
                fillDetailedBins(frame, fftOut, binToFreq);
            }
            if (mode == FftMode::BarsOnly || mode == FftMode::Both) {
                fillBarBins(frame, fftOut, binToFreq, decayRate);
            }

            frame.timestamp = m_newestBlockPts;

            emitFrame(frame);

            m_lastFrameAtSample += step;
        }

        if (!gotData && !st.stop_requested()) {
            m_consumer.waitFor(1, st);
        }
    }
}

bool DragonFftProcessor::drainPipeToHistory(std::stop_token st)
{
    (void)st;
    bool gotAny = false;
    const int ch = m_channelCount;

    while (auto available = m_consumer.ready()) {
        m_consumer.readSomeWith(available, [&](std::span<const DragonFftBlock> b1, std::span<const DragonFftBlock> b2) {
            auto processBlock = [&](const DragonFftBlock &blk) {
                m_newestBlockPts = blk.pts;

                for (size_t i = 0; i + ch <= blk.count; i += ch) {
                    std::float32_t sum = 0;
                    for (int c = 0; c < ch; ++c)
                        sum += blk.samples[i + c];
                    sum /= static_cast<std::float32_t>(ch);

                    m_sampleHistory[m_historyWritePos] = sum;
                    m_historyWritePos = (m_historyWritePos + 1) % m_sampleHistory.size();
                    ++m_historyTotalSamples;
                }
            };
            for (const auto &blk : b1)
                processBlock(blk);
            for (const auto &blk : b2)
                processBlock(blk);
        });
        gotAny = true;
    }
    return gotAny;
}

void DragonFftProcessor::readWindowEndingAt(const size_t endPos, std::span<std::float32_t, FFT_SIZE> out)
{
    // endPos is the monotonic sample index of the last sample in the window.
    // The window spans [endPos - FFT_SIZE + 1, endPos].
    const size_t startPos = endPos + 1 - FFT_SIZE;
    size_t circularPos = (m_historyWritePos + m_sampleHistory.size() + startPos - m_historyTotalSamples) % m_sampleHistory.size();

    for (auto &element : out) {
        element = m_sampleHistory[circularPos];
        circularPos = (circularPos + 1) % m_sampleHistory.size();
    }
}

bool DragonFftProcessor::historyHasEnoughForWindow() const
{
    return m_historyTotalSamples >= FFT_SIZE;
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

void DragonFftProcessor::fillBarBins(DragonFftFrame &frame, std::span<const std::complex<float>, FFT_SIZE / 2> fftOut, float binToFreq, float decayRate)
{
    const float melMin = hzToMel(MIN_FREQ);
    const float melMax = hzToMel(std::min(MAX_FREQ, static_cast<float>(m_sampleRate.load(std::memory_order_relaxed)) / 2.0f));

    std::array<std::float32_t, NUM_BAR_BINS> barBins{};
    for (auto [i, bin] : std::views::enumerate(barBins)) {
        const float t0 = static_cast<float>(i) / static_cast<float>(NUM_BAR_BINS);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(NUM_BAR_BINS);
        bin = computeMelBin(fftOut, binToFreq, melMin, melMax, t0, t1);
    }

    for (auto [prev, curr] : std::views::zip(m_prevBarFrequencies, barBins)) {
        prev = std::max(curr, prev - decayRate);
        curr = prev;
    }

    frame.barData.assign_range(barBins);
}

void DragonFftProcessor::emitFrame(const DragonFftFrame &frame)
{
    {
        std::scoped_lock lock(m_frameMutex);
        m_latestFrame = frame;
    }

    if (m_frameCallback) {
        m_frameCallback(frame);
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