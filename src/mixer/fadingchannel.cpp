/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "fadingchannel.h"
#include "player/pipe.h"

#include <algorithm>
#include <ranges>

void FadingChannel::install(const FadingSpec &spec)
{
    if (auto *oldFading = m_pipe.exchange(spec.pipe, std::memory_order_acq_rel)) {
        if (oldFading != spec.pipe) {
            oldFading->consumer().drain();
        }
    }

    if (spec.pipe != nullptr) {
        m_fade.configure({
            .duration = spec.fadeOut,
            .holdMax = spec.holdMax,
            .initialGain = spec.initialGain,
            .incomingHasFadeIn = spec.incomingHasFadeIn,
            .sampleRate = spec.sampleRate,
        });
    } else {
        m_fade.reset();
    }
}

void FadingChannel::retire() noexcept
{
    if (auto *fp = m_pipe.exchange(nullptr, std::memory_order_acq_rel)) {
        fp->consumer().drain();
    }
    m_fade.reset();
}

void FadingChannel::reset() noexcept
{
    m_pipe.store(nullptr, std::memory_order_release);
    m_fade.reset();
}

bool FadingChannel::isActive() const noexcept
{
    return m_pipe.load(std::memory_order_acquire) != nullptr;
}

bool FadingChannel::isFadeOutActive() const noexcept
{
    return m_fade.isActive();
}

bool FadingChannel::hasPipe(const Pipe<float> *pipe) const noexcept
{
    return pipe != nullptr && m_pipe.load(std::memory_order_acquire) == pipe;
}

float FadingChannel::currentGain() const noexcept
{
    return m_fade.currentGain();
}

std::size_t FadingChannel::mixInto(std::span<float> callbackBuffer, std::size_t primaryRead, std::size_t budget, bool holdForSlowLoad, int channels)
{
    if (channels <= 0) {
        return 0;
    }
    auto *fp = m_pipe.load(std::memory_order_acquire);
    std::size_t fadingRead = 0;

    if (fp != nullptr && m_fade.isActive()) {
        const auto remainingFadeFrames = m_fade.remainingBudgetFrames(holdForSlowLoad);
        const size_t fadingBudget = std::min(budget, remainingFadeFrames * static_cast<size_t>(channels));

        fp->consumer().readSomeWith(fadingBudget, [&](std::span<const float> b1, std::span<const float> b2) {
            fadingRead = b1.size() + b2.size();
            if (fadingRead == 0) {
                return;
            }

            if (fadingRead > primaryRead) {
                std::ranges::fill(callbackBuffer.subspan(primaryRead, fadingRead - primaryRead), 0.0f);
            }

            const auto ch = static_cast<std::size_t>(channels);
            const std::size_t rem1 = b1.size() % ch;
            const auto b1Aligned = b1.first(b1.size() - rem1);
            if (!b1Aligned.empty()) {
                const auto frames1 = static_cast<std::int64_t>(b1Aligned.size() / ch);
                mixWithGainRamp(callbackBuffer.subspan(0, b1Aligned.size()), b1Aligned, channels, m_fade.advanceChunk(frames1, holdForSlowLoad));
            }

            std::span<const float> b2Rest = b2;
            std::size_t b2Offset = b1.size();
            if (rem1 > 0) {
                const std::size_t need = std::min(ch - rem1, b2.size());
                const auto seamRamp = m_fade.advanceChunk((rem1 + need == ch) ? 1 : 0, holdForSlowLoad);
                const GainRamp seamGain{.start = seamRamp.start, .end = seamRamp.start};
                mixWithGainRamp(callbackBuffer.subspan(b1Aligned.size(), rem1), b1.last(rem1), channels, seamGain);
                if (need > 0) {
                    mixWithGainRamp(callbackBuffer.subspan(b1.size(), need), b2.first(need), channels, seamGain);
                    b2Rest = b2.subspan(need);
                    b2Offset += need;
                }
            }

            if (!b2Rest.empty()) {
                const auto frames2 = static_cast<std::int64_t>(b2Rest.size() / ch);
                mixWithGainRamp(callbackBuffer.subspan(b2Offset, b2Rest.size()), b2Rest, channels, m_fade.advanceChunk(frames2, holdForSlowLoad));
            }
        });

        if (fadingRead == 0 && !fp->producerDone()) {
            const auto budgetFrames = static_cast<std::int64_t>(fadingBudget / static_cast<size_t>(channels));
            std::ignore = m_fade.advanceChunk(budgetFrames, holdForSlowLoad);
        }
    }

    if (fp != nullptr && (!m_fade.isActive() || (fadingRead == 0 && fp->producerDone()))) {
        retire();
    }

    return fadingRead;
}
