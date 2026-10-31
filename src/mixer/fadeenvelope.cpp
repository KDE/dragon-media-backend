/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "fadeenvelope.h"

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace std::chrono_literals;

namespace
{

[[nodiscard]] std::int64_t msToFrames(std::chrono::milliseconds duration, int sampleRate) noexcept
{
    const auto ms = duration.count();
    if (ms <= 0 || sampleRate <= 0) {
        return 0;
    }
    return std::max<std::int64_t>(1, (ms * sampleRate) / 1000);
}

void runLinearRamp(std::size_t frames, int channels, GainRamp ramp, auto &&stereoFn, auto &&sampleFn) noexcept
{
    const float gainStep = (ramp.end - ramp.start) / static_cast<float>(frames);
    if (channels == 2) {
        for (std::size_t f = 0; f < frames; ++f) {
            const float g = ramp.start + static_cast<float>(f) * gainStep;
            stereoFn(2 * f, g);
        }
        return;
    }

    const auto ch = static_cast<std::size_t>(channels);
    for (std::size_t f = 0; f < frames; ++f) {
        const float g = ramp.start + static_cast<float>(f) * gainStep;
        const std::size_t frameOffset = f * ch;
        for (std::size_t c = 0; c < ch; ++c) {
            sampleFn(frameOffset + c, g);
        }
    }
}

}

FadeCurve::FadeCurve(FadeDirection direction) noexcept
    : m_direction(direction)
{
}

void FadeCurve::configure(FadeDirection direction, std::chrono::milliseconds duration, int sampleRate, float peakGain) noexcept
{
    m_direction = direction;
    m_peakGain = std::clamp(peakGain, 0.0f, 1.0f);
    m_totalFrames = msToFrames(duration, sampleRate);
    m_posFrames = 0;
}

void FadeCurve::reset(FadeDirection direction) noexcept
{
    m_direction = direction;
    m_peakGain = 1.0f;
    m_totalFrames = 0;
    m_posFrames = 0;
}

bool FadeCurve::isActive() const noexcept
{
    return m_totalFrames > 0 && m_posFrames < m_totalFrames;
}

std::int64_t FadeCurve::totalFrames() const noexcept
{
    return m_totalFrames;
}

std::int64_t FadeCurve::positionFrames() const noexcept
{
    return m_posFrames;
}

std::int64_t FadeCurve::remainingFrames() const noexcept
{
    return isActive() ? (m_totalFrames - m_posFrames) : 0;
}

float FadeCurve::restingGain() const noexcept
{
    return (m_direction == FadeDirection::In) ? m_peakGain : 0.0f;
}

float FadeCurve::gainAt(std::int64_t frame) const noexcept
{
    if (frame <= 0) {
        return (m_direction == FadeDirection::In) ? 0.0f : m_peakGain;
    }
    if (frame >= m_totalFrames) {
        return restingGain();
    }
    constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
    const float t = static_cast<float>(frame) / static_cast<float>(m_totalFrames);
    const float curve = (m_direction == FadeDirection::In) ? std::sin(t * kHalfPi) : std::cos(t * kHalfPi);
    return m_peakGain * curve;
}

float FadeCurve::currentGain() const noexcept
{
    if (!isActive()) {
        return restingGain();
    }
    return gainAt(m_posFrames);
}

GainRamp FadeCurve::advance(std::int64_t chunkFrames) noexcept
{
    if (!isActive() || chunkFrames <= 0) {
        const float g = currentGain();
        return {.start = g, .end = g};
    }
    const std::int64_t p0 = m_posFrames;
    m_posFrames = std::min(m_totalFrames, m_posFrames + chunkFrames);
    return {
        .start = gainAt(p0),
        .end = gainAt(m_posFrames),
    };
}

void FadeInEnvelope::configure(std::chrono::milliseconds duration, int sampleRate) noexcept
{
    m_duration = std::max(std::chrono::milliseconds{0}, duration);
    m_curve.configure(FadeDirection::In, m_duration, sampleRate);
}

void FadeInEnvelope::onFormatReopen(int sampleRate) noexcept
{
    if (m_duration > std::chrono::milliseconds{0} && m_curve.positionFrames() == 0 && sampleRate > 0) {
        m_curve.configure(FadeDirection::In, m_duration, sampleRate);
    } else {
        reset();
    }
}

void FadeInEnvelope::reset() noexcept
{
    m_duration = std::chrono::milliseconds{0};
    m_curve.reset(FadeDirection::In);
}

bool FadeInEnvelope::isActive() const noexcept
{
    return m_curve.isActive();
}

bool FadeInEnvelope::isWaitingForFirstSample() const noexcept
{
    return m_curve.isActive() && m_curve.positionFrames() == 0;
}

std::int64_t FadeInEnvelope::totalFrames() const noexcept
{
    return m_curve.totalFrames();
}

std::int64_t FadeInEnvelope::positionFrames() const noexcept
{
    return m_curve.positionFrames();
}

float FadeInEnvelope::currentGain() const noexcept
{
    return m_curve.currentGain();
}

GainRamp FadeInEnvelope::advanceChunk(std::int64_t chunkFrames) noexcept
{
    return m_curve.advance(chunkFrames);
}

void FadeOutEnvelope::configure(const Config &config) noexcept
{
    m_curve.configure(FadeDirection::Out, config.duration, config.sampleRate, config.initialGain);
    if (m_curve.isActive() && config.incomingHasFadeIn && config.holdMax > 0ms && config.sampleRate > 0) {
        m_holdRemainingFrames = (config.holdMax.count() * config.sampleRate) / 1000;
    } else {
        m_holdRemainingFrames = 0;
    }
}

void FadeOutEnvelope::reset() noexcept
{
    m_curve.reset(FadeDirection::Out);
    m_holdRemainingFrames = 0;
}

bool FadeOutEnvelope::isActive() const noexcept
{
    return m_curve.isActive();
}

std::int64_t FadeOutEnvelope::totalFrames() const noexcept
{
    return m_curve.totalFrames();
}

std::int64_t FadeOutEnvelope::positionFrames() const noexcept
{
    return m_curve.positionFrames();
}

std::int64_t FadeOutEnvelope::holdRemainingFrames() const noexcept
{
    return m_holdRemainingFrames;
}

std::size_t FadeOutEnvelope::remainingBudgetFrames(bool holdEnvelope) const noexcept
{
    if (!m_curve.isActive()) {
        return 0;
    }
    return static_cast<std::size_t>(m_curve.remainingFrames() + (holdEnvelope ? m_holdRemainingFrames : 0));
}

float FadeOutEnvelope::currentGain() const noexcept
{
    return m_curve.currentGain();
}

GainRamp FadeOutEnvelope::advanceChunk(std::int64_t chunkFrames, bool holdEnvelope) noexcept
{
    if (!m_curve.isActive() || chunkFrames <= 0) {
        const float g = m_curve.currentGain();
        return {.start = g, .end = g};
    }
    if (holdEnvelope && m_holdRemainingFrames > 0) {
        if (m_holdRemainingFrames >= chunkFrames) {
            m_holdRemainingFrames -= chunkFrames;
            const float g = m_curve.currentGain();
            return {.start = g, .end = g};
        }
        m_holdRemainingFrames = 0;
    }
    return m_curve.advance(chunkFrames);
}

void applyGainRamp(std::span<float> dst, int channels, GainRamp ramp) noexcept
{
    if (channels <= 0 || dst.empty()) {
        return;
    }
    if (ramp.start == ramp.end) {
        if (ramp.start == 1.0f) {
            return;
        }
        if (ramp.start == 0.0f) {
            std::ranges::fill(dst, 0.0f);
            return;
        }
        const float g = ramp.start;
        for (float &sample : dst) {
            sample *= g;
        }
        return;
    }

    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t frames = dst.size() / ch;
    if (frames > 0) {
        runLinearRamp(
            frames,
            channels,
            ramp,
            [dst](std::size_t base, float g) noexcept {
                dst[base] *= g;
                dst[base + 1] *= g;
            },
            [dst](std::size_t idx, float g) noexcept {
                dst[idx] *= g;
            });
    }
    for (std::size_t i = frames * ch; i < dst.size(); ++i) {
        dst[i] *= ramp.end;
    }
}

void mixWithGainRamp(std::span<float> dst, std::span<const float> src, int channels, GainRamp ramp) noexcept
{
    const std::size_t sampleCount = std::min(dst.size(), src.size());
    if (channels <= 0 || sampleCount == 0) {
        return;
    }
    if (ramp.start == ramp.end) {
        if (ramp.start == 0.0f) {
            return;
        }
        if (ramp.start == 1.0f) {
            for (std::size_t i = 0; i < sampleCount; ++i) {
                dst[i] = std::clamp(dst[i] + src[i], -1.0f, 1.0f);
            }
            return;
        }
        const float g = ramp.start;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            dst[i] = std::clamp(dst[i] + src[i] * g, -1.0f, 1.0f);
        }
        return;
    }

    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t frames = sampleCount / ch;
    if (frames > 0) {
        runLinearRamp(
            frames,
            channels,
            ramp,
            [dst, src](std::size_t base, float g) noexcept {
                dst[base] = std::clamp(dst[base] + src[base] * g, -1.0f, 1.0f);
                dst[base + 1] = std::clamp(dst[base + 1] + src[base + 1] * g, -1.0f, 1.0f);
            },
            [dst, src](std::size_t idx, float g) noexcept {
                dst[idx] = std::clamp(dst[idx] + src[idx] * g, -1.0f, 1.0f);
            });
    }
    for (std::size_t i = frames * ch; i < sampleCount; ++i) {
        dst[i] = std::clamp(dst[i] + src[i] * ramp.end, -1.0f, 1.0f);
    }
}
