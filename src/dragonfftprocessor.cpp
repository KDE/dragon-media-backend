/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragonfftprocessor.h>
#include <stdfloat>

#include <LockFreeSpscQueue.h>
#include <kissfft.hh>

#include <QDebug>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <numbers>
#include <ranges>
#include <thread>

using namespace std::chrono_literals;

DragonFftProcessor::DragonFftProcessor()
    : m_fft(std::make_unique<kissfft<float>>(FFT_SIZE / 2, false))
    , m_inputWindow{}
    , m_prevBarFrequencies{}
{
    m_prevBarFrequencies.fill(-80.0f);
}

DragonFftProcessor::~DragonFftProcessor() = default;

void DragonFftProcessor::setQueue(LockFreeSpscQueue<std::float32_t> *queue)
{
    m_fftQueue = queue;
}

void DragonFftProcessor::setWaitCv(std::condition_variable *cv)
{
    m_waitCv = cv;
}

void DragonFftProcessor::setSampleRate(int sampleRate)
{
    m_sampleRate = sampleRate;
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
    std::lock_guard lock(m_frameMutex);
    DragonFftFrame result = std::move(m_latestFrame);
    m_latestFrame = {};
    return result;
}

void DragonFftProcessor::processLoop(std::stop_token st)
{
    int frameCount = 0;
    while (!st.stop_requested()) {
        if (m_waitCv) {
            std::unique_lock lock(m_waitMutex);
            m_waitCv->wait_for(lock, std::chrono::milliseconds(50), [&] {
                return !m_fftQueue || m_fftQueue->get_num_items_ready() >= FFT_SIZE || st.stop_requested();
            });
        }

        if (st.stop_requested())
            break;
        if (!m_fftQueue || m_fftQueue->get_num_items_ready() < FFT_SIZE)
            continue;

        auto scope = m_fftQueue->prepare_read(FFT_SIZE);
        assert(scope.get_items_read() == FFT_SIZE);
        ++frameCount;

        auto block1 = scope.get_block1();
        auto block2 = scope.get_block2();
        auto [_, out1] = std::ranges::copy(block1, m_inputWindow.begin());
        std::ranges::copy(block2, out1);

        if (st.stop_requested()) {
            break;
        }

        applyHannWindow(m_inputWindow);

        std::array<std::complex<float>, FFT_SIZE / 2> fftOut;
        transformReal(m_inputWindow, fftOut);

        const float binToFreq = static_cast<float>(m_sampleRate) / static_cast<float>(FFT_SIZE);

        auto getMag = [&](const int idx) -> float {
            if (idx < 0 || idx >= static_cast<int>(fftOut.size())) {
                return 0.0f;
            }
            float mag;
            if (idx == 0) {
                mag = std::abs(fftOut[0].real()) / static_cast<float>(FFT_SIZE);
            } else {
                mag = std::abs(fftOut[static_cast<size_t>(idx)]) / static_cast<float>(FFT_SIZE);
            }

            const float freq = static_cast<float>(idx) * binToFreq;
            const float tilt = std::sqrt(std::max(freq, MIN_FREQ) / MIN_FREQ);
            return mag * tilt;
        };

        const float melMin = hzToMel(MIN_FREQ);
        const float melMax = hzToMel(std::min(MAX_FREQ, static_cast<float>(m_sampleRate) / 2.0f));

        auto computeBin = [&](float t0, float t1) -> float {
            const float freq0 = melToHz(melMin + t0 * (melMax - melMin));
            const float freq1 = melToHz(melMin + t1 * (melMax - melMin));

            const float binIdx0 = freq0 / binToFreq;
            const float binIdx1 = freq1 / binToFreq;

            const int startBin = static_cast<int>(std::floor(binIdx0));
            const int endBin = static_cast<int>(std::ceil(binIdx1));

            float maxMag = 0.0f;
            if (endBin <= startBin + 1) {
                const float frac = binIdx0 - static_cast<float>(startBin);
                maxMag = std::lerp(getMag(startBin), getMag(startBin + 1), frac);
            } else {
                auto binRange = std::views::iota(startBin, std::min(endBin, static_cast<int>(fftOut.size())));
                if (!std::ranges::empty(binRange)) {
                    maxMag = std::ranges::max(binRange | std::views::transform(getMag));
                }
            }

            return 20.0f * std::log10(std::max(maxMag, 1e-6f));
        };

        std::array<std::float32_t, NUM_LOG_BINS> logBins;
        for (auto [i, bin] : std::views::enumerate(logBins)) {
            const float t0 = static_cast<float>(i) / static_cast<float>(NUM_LOG_BINS);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(NUM_LOG_BINS);
            bin = computeBin(t0, t1);
        }

        std::array<std::float32_t, NUM_BAR_BINS> barBins;
        for (auto [i, bin] : std::views::enumerate(barBins)) {
            const float t0 = static_cast<float>(i) / static_cast<float>(NUM_BAR_BINS);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(NUM_BAR_BINS);
            bin = computeBin(t0, t1);
        }

        constexpr float decayRate = 1.5f;
        for (auto [prev, curr] : std::views::zip(m_prevBarFrequencies, barBins)) {
            prev = std::max(curr, prev - decayRate);
            curr = prev;
        }

        DragonFftFrame frame;
        frame.frequenciesDb.assign_range(logBins);
        frame.barData.assign_range(barBins);
        frame.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());

        {
            std::lock_guard lock(m_frameMutex);
            m_latestFrame = frame;
        }

        if (m_frameCallback) {
            m_frameCallback(frame);
        }

        if (frameCount <= 3 || frameCount % 60 == 0) {
            qDebug() << "FFT: frame emitted count=" << frameCount << "barData[0]=" << frame.barData[0] << "barData[11]=" << frame.barData[11]
                     << "barData[23]=" << frame.barData[23];
        }
    }
}

void DragonFftProcessor::applyHannWindow(std::span<std::float32_t> data)
{
    const float size = static_cast<float>(data.size());
    for (auto [i, val] : std::views::enumerate(data)) {
        const float window = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / (size - 1.0f)));
        val *= window;
    }
}

constexpr float DragonFftProcessor::hzToMel(float f)
{
    return 2595.0f * std::log10(1.0f + f / 700.0f);
}

constexpr float DragonFftProcessor::melToHz(float m)
{
    return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f);
}