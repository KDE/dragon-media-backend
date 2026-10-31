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
 * Audio-thread primary (incoming/active) playback channel.
 *
 * Owns the active audio pipe and its equal-power fade-in envelope.
 */
class DRAGONMEDIABACKEND_EXPORT PrimaryChannel
{
public:
    PrimaryChannel() = default;
    ~PrimaryChannel() = default;

    PrimaryChannel(const PrimaryChannel &) = delete;
    PrimaryChannel &operator=(const PrimaryChannel &) = delete;
    PrimaryChannel(PrimaryChannel &&) = delete;
    PrimaryChannel &operator=(PrimaryChannel &&) = delete;

    void setPipe(Pipe<float> *pipe) noexcept;
    void install(Pipe<float> *pipe, std::chrono::milliseconds fadeIn, int sampleRate) noexcept;
    void onFormatReopen(int sampleRate) noexcept;
    void drain() noexcept;
    void resetFade() noexcept;

    // Pulls raw samples into dst; returns count read.
    std::size_t pull(std::span<float> dst, std::size_t budget);

    // Applies fade-in ramp in-place to dst.first(samplesRead).
    void applyFade(std::span<float> dst, std::size_t samplesRead, int channels) noexcept;

    [[nodiscard]] bool isFadeActive() const noexcept;
    [[nodiscard]] bool isWaitingForFirstSample() const noexcept;
    [[nodiscard]] float currentGain() const noexcept;
    [[nodiscard]] Pipe<float> *pipe() const noexcept
    {
        return m_pipe.load(std::memory_order_relaxed);
    }

private:
    std::atomic<Pipe<float> *> m_pipe{nullptr};
    FadeInEnvelope m_fade;
};
