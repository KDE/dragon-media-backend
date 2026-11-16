/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "voicechannel.h"
#include "player/pipe.h"

#include <algorithm>
#include <ranges>

VoiceChannel::VoiceChannel(Pipe<float> *pipe) noexcept
    : m_pipe(pipe)
{
}

VoiceChannel::~VoiceChannel() = default;

std::size_t VoiceChannel::pull(std::span<float> dst, std::size_t budget)
{
    if (m_pipe == nullptr || budget == 0) {
        return 0;
    }

    std::size_t samplesRead = 0;
    m_pipe->consumer().readSomeWith(budget, [&](std::span<const float> b1, std::span<const float> b2) {
        samplesRead = b1.size() + b2.size();
        if (samplesRead == 0) {
            return;
        }
        auto it = dst.begin();
        it = std::ranges::copy(b1, it).out;
        std::ranges::copy(b2, it);
    });
    return samplesRead;
}

void VoiceChannel::configureFadeIn(std::chrono::milliseconds duration, int sampleRate) noexcept
{
    m_fadeIn.configure(duration, sampleRate);
    publishGain();
}

void VoiceChannel::onFormatReopen(int sampleRate) noexcept
{
    m_fadeIn.onFormatReopen(sampleRate);
    publishGain();
}

void VoiceChannel::resetFadeIn() noexcept
{
    m_fadeIn.reset();
    publishGain();
}

void VoiceChannel::applyFadeIn(std::span<float> dst, std::size_t samplesRead, int channels) noexcept
{
    if (!m_fadeIn.isActive() || samplesRead == 0 || channels <= 0) {
        return;
    }

    const auto frames = static_cast<std::int64_t>(samplesRead / static_cast<std::size_t>(channels));
    if (frames <= 0) {
        return;
    }

    applyGainRamp(dst.first(samplesRead), channels, m_fadeIn.advanceChunk(frames));
    publishGain();
}

bool VoiceChannel::isFadeInActive() const noexcept
{
    return m_fadeIn.isActive();
}

bool VoiceChannel::isWaitingForFirstSample() const noexcept
{
    return m_fadeIn.isWaitingForFirstSample();
}

float VoiceChannel::currentGain() const noexcept
{
    return m_fadeIn.isActive() ? m_fadeIn.currentGain() : 1.0f;
}

void VoiceChannel::configureFadeOut(const FadeOutEnvelope::Config &config) noexcept
{
    m_fadeOut.configure(config);
}

void VoiceChannel::applyFadeOut(std::span<float> dst, std::size_t samplesRead, int channels, bool holdForSlowLoad) noexcept
{
    if (!m_fadeOut.isActive() || samplesRead == 0 || channels <= 0) {
        return;
    }

    const auto frames = static_cast<std::int64_t>(samplesRead / static_cast<std::size_t>(channels));
    if (frames <= 0) {
        return;
    }

    applyGainRamp(dst.first(samplesRead), channels, m_fadeOut.advanceChunk(frames, holdForSlowLoad));
}

void VoiceChannel::advanceFadeOutOverSilence(std::size_t budgetSamples, int channels, bool holdForSlowLoad) noexcept
{
    if (channels <= 0) {
        return;
    }
    const auto frames = static_cast<std::int64_t>(budgetSamples / static_cast<std::size_t>(channels));
    std::ignore = m_fadeOut.advanceChunk(frames, holdForSlowLoad);
}

bool VoiceChannel::isFadeOutActive() const noexcept
{
    return m_fadeOut.isActive();
}

std::size_t VoiceChannel::fadeOutBudgetSamples(std::size_t budget, int channels, bool holdForSlowLoad) const noexcept
{
    if (channels <= 0) {
        return 0;
    }
    const std::size_t remaining = m_fadeOut.remainingBudgetFrames(holdForSlowLoad) * static_cast<std::size_t>(channels);
    return std::min(budget, remaining);
}

void VoiceChannel::drainPipe() noexcept
{
    if (m_pipe != nullptr) {
        m_pipe->consumer().drain();
    }
}

bool VoiceChannel::producerDone() const noexcept
{
    return m_pipe != nullptr && m_pipe->producerDone();
}

void VoiceChannel::publishGain() noexcept
{
    m_currentGainMirror.store(currentGain(), std::memory_order_release);
}
