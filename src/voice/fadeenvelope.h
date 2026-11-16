/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

struct GainRamp {
    float start = 1.0f;
    float end = 1.0f;
};

enum class FadeDirection : std::uint8_t {
    In,
    Out,
};

/**
 * Quarter-wave equal-power fade ramp shared by fade-in and fade-out envelopes.
 */
class DRAGONMEDIABACKEND_EXPORT FadeCurve
{
public:
    explicit FadeCurve(FadeDirection direction = FadeDirection::In) noexcept;

    void configure(FadeDirection direction, std::chrono::milliseconds duration, int sampleRate, float peakGain = 1.0f) noexcept;
    void reset(FadeDirection direction) noexcept;

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] std::int64_t totalFrames() const noexcept;
    [[nodiscard]] std::int64_t positionFrames() const noexcept;
    [[nodiscard]] std::int64_t remainingFrames() const noexcept;
    [[nodiscard]] float currentGain() const noexcept;
    [[nodiscard]] GainRamp advance(std::int64_t chunkFrames) noexcept;

private:
    [[nodiscard]] float gainAt(std::int64_t frame) const noexcept;
    [[nodiscard]] float restingGain() const noexcept;

    std::int64_t m_totalFrames = 0;
    std::int64_t m_posFrames = 0;
    float m_peakGain = 1.0f;
    FadeDirection m_direction = FadeDirection::In;
};

class DRAGONMEDIABACKEND_EXPORT FadeInEnvelope
{
public:
    FadeInEnvelope() noexcept = default;

    void configure(std::chrono::milliseconds duration, int sampleRate) noexcept;
    void onFormatReopen(int sampleRate) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] bool isWaitingForFirstSample() const noexcept;
    [[nodiscard]] std::int64_t totalFrames() const noexcept;
    [[nodiscard]] float currentGain() const noexcept;
    [[nodiscard]] GainRamp advanceChunk(std::int64_t chunkFrames) noexcept;

private:
    FadeCurve m_curve{FadeDirection::In};
    std::chrono::milliseconds m_duration{0};
};

class DRAGONMEDIABACKEND_EXPORT FadeOutEnvelope
{
public:
    struct Config {
        std::chrono::milliseconds duration{0};
        std::chrono::milliseconds holdMax{0};
        float initialGain = 1.0f;
        bool incomingHasFadeIn = false;
        int sampleRate = 0;
    };

    FadeOutEnvelope() noexcept = default;

    void configure(const Config &config) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] std::int64_t totalFrames() const noexcept;
    [[nodiscard]] std::size_t remainingBudgetFrames(bool holdEnvelope) const noexcept;
    [[nodiscard]] float currentGain() const noexcept;
    [[nodiscard]] GainRamp advanceChunk(std::int64_t chunkFrames, bool holdEnvelope = false) noexcept;

private:
    FadeCurve m_curve{FadeDirection::Out};
    std::int64_t m_holdRemainingFrames = 0;
};

// Auto-vectorizing in-place linear gain ramp for one voice's rendered PCM.
DRAGONMEDIABACKEND_EXPORT void applyGainRamp(std::span<float> dst, int channels, GainRamp ramp) noexcept;
