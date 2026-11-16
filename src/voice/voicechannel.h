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
 * Audio-thread playback channel of one Voice: a fixed pipe, the equal-power
 * fade-in envelope, and the equal-power fade-out envelope (with slow-load
 * hold).
 *
 * The pipe identity is fixed for the channel's whole lifetime: a pipe has
 * exactly one Voice consumer for that Voice's entire lifetime. There is no
 * atomic pipe exchange and no mixing entry point; the audio server sums the
 * voices.
 *
 * Fade state is audio-thread confined, with one exception: configureFadeOut()
 * runs on the Qt main thread while arming a crossfade, which is why the
 * request is staged through Voice's atomic command block and only consumed
 * by the audio thread (see Voice::consumeFadeOutRequest).
 */
class DRAGONMEDIABACKEND_EXPORT VoiceChannel
{
public:
    explicit VoiceChannel(Pipe<float> *pipe) noexcept;

    VoiceChannel() = delete;
    ~VoiceChannel();

    VoiceChannel(const VoiceChannel &) = delete;
    VoiceChannel &operator=(const VoiceChannel &) = delete;
    VoiceChannel(VoiceChannel &&) = delete;
    VoiceChannel &operator=(VoiceChannel &&) = delete;

    [[nodiscard]] Pipe<float> *pipe() const noexcept
    {
        return m_pipe;
    }

    // Pull raw samples into dst (ring-wrap-safe, frame-aligned within the
    // budget); returns the count read.
    [[nodiscard]] std::size_t pull(std::span<float> dst, std::size_t budget);

    // Audio-thread or quiesced fades. The atomic gain mirror is read by the
    // main thread when arming a crossfade out of a partially faded-in voice.
    void configureFadeIn(std::chrono::milliseconds duration, int sampleRate) noexcept;
    void onFormatReopen(int sampleRate) noexcept;
    void resetFadeIn() noexcept;
    void applyFadeIn(std::span<float> dst, std::size_t samplesRead, int channels) noexcept;
    [[nodiscard]] bool isFadeInActive() const noexcept;
    [[nodiscard]] bool isWaitingForFirstSample() const noexcept;
    [[nodiscard]] float currentGain() const noexcept;

    // Audio thread only (consumed from Voice's command block).
    void configureFadeOut(const FadeOutEnvelope::Config &config) noexcept;
    void applyFadeOut(std::span<float> dst, std::size_t samplesRead, int channels, bool holdForSlowLoad) noexcept;
    // Explicit advancement over unpaused starvation quanta during fade-out:
    // the hold and fade budgets advance on real callback time even when the
    // retiring pipe is starved, so a stalled producer cannot freeze the fade.
    void advanceFadeOutOverSilence(std::size_t budgetSamples, int channels, bool holdForSlowLoad) noexcept;
    [[nodiscard]] bool isFadeOutActive() const noexcept;
    // Remaining pull budget (in samples) for the fade-out, including the
    // slow-load hold window when held.
    [[nodiscard]] std::size_t fadeOutBudgetSamples(std::size_t budget, int channels, bool holdForSlowLoad) const noexcept;

    void drainPipe() noexcept;
    [[nodiscard]] bool producerDone() const noexcept;

    // Mirrored for cross-thread reads of the partially faded-in gain; the
    // audio thread is the sole writer. Read through currentGainForCrossfade().
    [[nodiscard]] float currentGainForCrossfade() const noexcept
    {
        return m_currentGainMirror.load(std::memory_order_acquire);
    }

private:
    void publishGain() noexcept;

    Pipe<float> *const m_pipe;
    FadeInEnvelope m_fadeIn;
    FadeOutEnvelope m_fadeOut;
    std::atomic<float> m_currentGainMirror{1.0f};
};
