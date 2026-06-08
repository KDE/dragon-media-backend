/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiosink.h"
#include "dragonfftblock.h"
#include "dragonpipe.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{

float sliderToLinearGain(float sliderValue)
{
    sliderValue = std::clamp(sliderValue, 0.0f, 1.0f);

    if (sliderValue <= 0.001f) {
        return 0.0f;
    }

    if (sliderValue >= 0.99f) {
        return 1.0f;
    }

    constexpr float LOG100 = 4.60517018599f;
    return -std::log(1.0f - sliderValue) / LOG100;
}

float calculateGain(float volume, bool muted)
{
    return muted ? 0.0f : sliderToLinearGain(volume);
}

float linearGainToSlider(float linearGain)
{
    if (linearGain <= 0.0f) {
        return 0.0f;
    }
    if (linearGain >= 1.0f) {
        return 1.0f;
    }
    constexpr float LOG100 = 4.60517018599f;
    return 1.0f - std::exp(-linearGain * LOG100);
}
}

DragonAudioSink::DragonAudioSink(QObject *parent)
    : QObject(parent)
{
}

DragonAudioSink::~DragonAudioSink() = default;

void DragonAudioSink::setAudioPipe(DragonPipe<std::float32_t> *pipe)
{
    m_audioPipe.store(pipe, std::memory_order_release);
}

void DragonAudioSink::setFftPipe(DragonPipe<DragonFftBlock> *pipe)
{
    m_fftPipe.store(pipe, std::memory_order_release);
}

void DragonAudioSink::setQueueReady(bool ready)
{
    m_queueReady.store(ready, std::memory_order_release);
}

bool DragonAudioSink::isQueueReady() const
{
    return m_queueReady.load(std::memory_order_acquire);
}

void DragonAudioSink::setPositionOffset(int64_t offsetMs, PositionResetMode mode)
{
    m_positionOffsetMs.store(offsetMs, std::memory_order_relaxed);

    if (isPaused()) {
        if (mode == PositionResetMode::Seek || mode == PositionResetMode::NormalTrackChange) {
            auto *pipe = m_audioPipe.load(std::memory_order_acquire);
            if (pipe) {
                pipe->consumer().drain();
            }
            close();
            setQueueReady(true);
        }
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        return;
    }

    switch (mode) {
    case PositionResetMode::Seek:
    case PositionResetMode::NormalTrackChange:
        m_flushPending.store(true, std::memory_order_release);
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    case PositionResetMode::GaplessTransition:
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    }
}

float DragonAudioSink::volume() const
{
    return m_volume;
}

void DragonAudioSink::setVolume(float volume)
{
    float clampedVolume = std::clamp(volume, 0.0f, 1.0f);
    if (qAbs(m_volume - clampedVolume) < 0.001f) {
        return;
    }
    m_volume = clampedVolume;
    setGain(calculateGain(m_volume, m_muted));
    Q_EMIT volumeChanged();
}

bool DragonAudioSink::muted() const
{
    return m_muted;
}

void DragonAudioSink::setMuted(bool muted)
{
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    setGain(calculateGain(m_volume, m_muted));
    Q_EMIT volumeChanged();
}

void DragonAudioSink::onExternalVolumeChanged(float linearGain)
{
    setVolume(linearGainToSlider(linearGain));
}

void DragonAudioSink::setStreamName(const QString &name)
{
    Q_UNUSED(name);
}

int64_t DragonAudioSink::positionMs() const
{
    if (m_channels <= 0 || m_sampleRate <= 0) {
        return m_positionOffsetMs.load(std::memory_order_relaxed);
    }

    int64_t written = m_totalSamplesWritten.load(std::memory_order_relaxed);

    const int64_t queued = deviceQueuedSamples();
    if (queued > 0) {
        written = std::max(int64_t{0}, written - queued);
    }

    const int64_t frameCount = written / m_channels;
    return (frameCount * 1000 / m_sampleRate) + m_positionOffsetMs.load(std::memory_order_relaxed);
}

bool DragonAudioSink::hasFormat(int sampleRate, int channels) const
{
    return m_sampleRate == sampleRate && m_channels == channels;
}

int64_t DragonAudioSink::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

void DragonAudioSink::resetPositionTracking()
{
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    m_positionOffsetMs.store(0, std::memory_order_relaxed);
}

void DragonAudioSink::silence()
{
    setGain(0.0f);
}

void DragonAudioSink::restoreVolume()
{
    setGain(calculateGain(m_volume, m_muted));
}

std::span<const std::float32_t> DragonAudioSink::processAudioCallback(size_t maxSamples, std::chrono::microseconds estimatedPts)
{
    if (m_positionResetPending.exchange(false, std::memory_order_acq_rel)) {
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

    if (m_flushPending.exchange(false, std::memory_order_acq_rel)) {
        if (auto *ap = m_audioPipe.load(std::memory_order_acquire)) {
            ap->consumer().drain();
        }
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        m_queueReady.store(true, std::memory_order_release);
        return {};
    }

    auto *ap = m_audioPipe.load(std::memory_order_acquire);
    if (!ap) {
        return {};
    }

    m_callbackBuffer.resize(maxSamples);
    size_t totalRead = 0;

    ap->consumer().readSomeWith(maxSamples, [&](std::span<const std::float32_t> b1, std::span<const std::float32_t> b2) {
        totalRead = b1.size() + b2.size();
        if (totalRead == 0) {
            return;
        }

        auto it = m_callbackBuffer.begin();
        it = std::ranges::copy(b1, it).out;
        std::ranges::copy(b2, it);
    });

    if (totalRead == 0) {
        return {};
    }

    const auto consumedSpan = std::span{m_callbackBuffer.data(), totalRead};

    if (auto *fp = m_fftPipe.load(std::memory_order_acquire)) {
        size_t blocksNeeded = (totalRead + DragonFftBlock::MAX_SAMPLES - 1) / DragonFftBlock::MAX_SAMPLES;
        fp->producer().writeSomeWith(blocksNeeded, [&](std::span<DragonFftBlock> fb1, std::span<DragonFftBlock> fb2) {
            size_t srcOffset = 0;
            auto fillBlock = [&](std::span<DragonFftBlock> dst) {
                for (size_t i = 0; i < dst.size() && srcOffset < totalRead; ++i) {
                    size_t remaining = totalRead - srcOffset;
                    size_t toCopy = std::min(remaining, DragonFftBlock::MAX_SAMPLES);
                    dst[i].count = toCopy;
                    dst[i].pts = estimatedPts;

                    for (size_t j = 0; j < toCopy; ++j) {
                        dst[i].samples[j] = m_callbackBuffer[srcOffset + j];
                    }
                    srcOffset += toCopy;
                }
            };
            fillBlock(fb1);
            fillBlock(fb2);
        });
    }

    m_totalSamplesWritten.fetch_add(static_cast<int64_t>(totalRead), std::memory_order_relaxed);

    return consumedSpan;
}

DragonPipe<std::float32_t> *DragonAudioSink::audioPipe() const
{
    return m_audioPipe.load(std::memory_order_acquire);
}

DragonPipe<DragonFftBlock> *DragonAudioSink::fftPipe() const
{
    return m_fftPipe.load(std::memory_order_acquire);
}

void DragonAudioSink::setFormat(int sampleRate, int channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
}
