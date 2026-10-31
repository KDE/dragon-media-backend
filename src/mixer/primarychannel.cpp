/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "primarychannel.h"
#include "player/pipe.h"

#include <ranges>

void PrimaryChannel::setPipe(Pipe<float> *pipe) noexcept
{
    m_pipe.store(pipe, std::memory_order_release);
}

void PrimaryChannel::install(Pipe<float> *pipe, std::chrono::milliseconds fadeIn, int sampleRate) noexcept
{
    if (pipe != nullptr) {
        m_pipe.store(pipe, std::memory_order_release);
    }
    m_fade.configure(fadeIn, sampleRate);
}

void PrimaryChannel::onFormatReopen(int sampleRate) noexcept
{
    m_fade.onFormatReopen(sampleRate);
}

void PrimaryChannel::drain() noexcept
{
    if (auto *p = m_pipe.load(std::memory_order_acquire)) {
        p->consumer().drain();
    }
}

void PrimaryChannel::resetFade() noexcept
{
    m_fade.reset();
}

std::size_t PrimaryChannel::pull(std::span<float> dst, std::size_t budget)
{
    auto *pipe = m_pipe.load(std::memory_order_acquire);
    if (pipe == nullptr) {
        return 0;
    }

    std::size_t samplesRead = 0;
    pipe->consumer().readSomeWith(budget, [&](std::span<const float> b1, std::span<const float> b2) {
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

void PrimaryChannel::applyFade(std::span<float> dst, std::size_t samplesRead, int channels) noexcept
{
    if (!m_fade.isActive() || samplesRead == 0 || channels <= 0) {
        return;
    }

    const auto frames = static_cast<std::int64_t>(samplesRead / static_cast<std::size_t>(channels));
    if (frames <= 0) {
        return;
    }

    applyGainRamp(dst.first(samplesRead), channels, m_fade.advanceChunk(frames));
}

bool PrimaryChannel::isFadeActive() const noexcept
{
    return m_fade.isActive();
}

bool PrimaryChannel::isWaitingForFirstSample() const noexcept
{
    return m_fade.isWaitingForFirstSample();
}

float PrimaryChannel::currentGain() const noexcept
{
    return m_fade.currentGain();
}
