/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "sink/producerstate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

/**
 * Atomic one-shot slot for a staged flush command.
 *
 * Encapsulates a 64-bit atomic word containing:
 * - 1 bit pending flag (bit 63; zero word means no flush is pending)
 * - 20 bits epoch (bits 43..62)
 * - 43 bits fade-in duration in milliseconds (bits 0..42)
 */
class StagedFlush
{
public:
    struct Command {
        std::uint64_t epoch = 0;
        std::chrono::milliseconds fadeIn{0};
    };

    void store(std::uint64_t epoch, std::chrono::milliseconds fadeIn) noexcept
    {
        const auto clampedMs = std::clamp<std::int64_t>(fadeIn.count(), 0, static_cast<std::int64_t>(kFadeInMask));
        m_word.store(pack(epoch, clampedMs), std::memory_order_release);
    }

    [[nodiscard]] std::optional<Command> take() noexcept
    {
        const auto word = m_word.exchange(0, std::memory_order_acq_rel);
        if (word == 0) {
            return std::nullopt;
        }
        return Command{
            .epoch = unpackEpoch(word),
            .fadeIn = unpackFadeIn(word),
        };
    }

    void clear() noexcept
    {
        m_word.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool isPending() const noexcept
    {
        return m_word.load(std::memory_order_acquire) != 0;
    }

private:
    static constexpr std::uint64_t kFadeInBits = 43;
    static constexpr std::uint64_t kFadeInMask = (std::uint64_t{1} << kFadeInBits) - 1;
    static constexpr std::uint64_t kEpochShift = kFadeInBits;
    static constexpr std::uint64_t kEpochMask = ProducerState::kEpochMask;
    static constexpr std::uint64_t kPendingBit = std::uint64_t{1} << 63;

    static constexpr std::uint64_t pack(std::uint64_t epoch, std::int64_t fadeInMs) noexcept
    {
        return kPendingBit | ((epoch & kEpochMask) << kEpochShift) | (static_cast<std::uint64_t>(fadeInMs) & kFadeInMask);
    }

    static constexpr std::uint64_t unpackEpoch(std::uint64_t word) noexcept
    {
        return (word >> kEpochShift) & kEpochMask;
    }

    static constexpr std::chrono::milliseconds unpackFadeIn(std::uint64_t word) noexcept
    {
        return std::chrono::milliseconds{static_cast<std::int64_t>(word & kFadeInMask)};
    }

    std::atomic<std::uint64_t> m_word{0};
};
