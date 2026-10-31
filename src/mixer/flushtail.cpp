/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "flushtail.h"
#include "fadeenvelope.h"
#include "player/pipe.h"
#include "primarychannel.h"

#include <algorithm>

void FlushTail::capture(PrimaryChannel &primary, int sampleRate, int channels)
{
    auto *pipe = primary.pipe();
    if (pipe == nullptr || sampleRate <= 0 || channels <= 0) {
        return;
    }

    const float startGain = primary.currentGain();
    if (startGain <= 0.0f) {
        clear();
        return;
    }

    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t targetFrames = std::min<std::size_t>(m_buffer.size() / ch, static_cast<std::size_t>((sampleRate * 5) / 1000));
    const std::size_t targetSamples = targetFrames * ch;
    if (targetSamples == 0) {
        return;
    }

    pipe->consumer().readSomeWith(targetSamples, [&](std::span<const float> b1, std::span<const float> b2) {
        const std::size_t alignedCount = ((b1.size() + b2.size()) / ch) * ch;
        if (alignedCount == 0) {
            return;
        }
        m_offset = 0;
        m_count = alignedCount;
        const std::size_t copy1 = std::min(b1.size(), alignedCount);
        const std::size_t copy2 = alignedCount - copy1;
        auto out = std::ranges::copy(b1.first(copy1), m_buffer.begin()).out;
        std::ranges::copy(b2.first(copy2), out);
        applyGainRamp(std::span{m_buffer}.first(m_count), channels, {.start = startGain, .end = 0.0f});
    });
}

std::size_t FlushTail::mixInto(std::span<float> dst, std::size_t validSamples, int channels) noexcept
{
    validSamples = std::min(validSamples, dst.size());
    if (channels <= 0) {
        return validSamples;
    }

    const std::size_t remaining = m_count - m_offset;
    const std::size_t toMix = std::min(remaining, dst.size());
    if (toMix == 0) {
        return validSamples;
    }
    if (toMix > validSamples) {
        std::ranges::fill(dst.subspan(validSamples, toMix - validSamples), 0.0f);
    }
    const auto tail = std::span<const float>{m_buffer}.subspan(m_offset, toMix);
    mixWithGainRamp(dst.first(toMix), tail, channels, {.start = 1.0f, .end = 1.0f});
    m_offset += toMix;
    return std::max(validSamples, toMix);
}

void FlushTail::clear() noexcept
{
    m_offset = 0;
    m_count = 0;
}
