/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include "fadeenvelope.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>

template<typename T>
class Pipe;

/**
 * Audio-thread fading (outgoing) playback channel.
 *
 * Owns the retiring audio pipe and the equal-power fade-out envelope
 * (with slow-load hold).
 */
class DRAGONMEDIABACKEND_EXPORT FadingChannel
{
public:
    FadingChannel() = default;
    ~FadingChannel() = default;

    FadingChannel(const FadingChannel &) = delete;
    FadingChannel &operator=(const FadingChannel &) = delete;
    FadingChannel(FadingChannel &&) = delete;
    FadingChannel &operator=(FadingChannel &&) = delete;

    struct FadingSpec {
        Pipe<float> *pipe = nullptr;
        std::chrono::milliseconds fadeOut{0};
        std::chrono::milliseconds holdMax{0};
        float initialGain = 1.0f;
        bool incomingHasFadeIn = false;
        int sampleRate = 0;
    };

    void install(const FadingSpec &spec);

    void retire() noexcept;
    void reset() noexcept;

    std::size_t mixInto(std::span<float> callbackBuffer, std::size_t primaryRead, std::size_t budget, bool holdForSlowLoad, int channels);

    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] bool isFadeOutActive() const noexcept;
    [[nodiscard]] bool hasPipe(const Pipe<float> *pipe) const noexcept;
    [[nodiscard]] float currentGain() const noexcept;
    [[nodiscard]] Pipe<float> *pipe() const noexcept
    {
        return m_pipe.load(std::memory_order_relaxed);
    }

private:
    std::atomic<Pipe<float> *> m_pipe{nullptr};
    FadeOutEnvelope m_fade;
};
