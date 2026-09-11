/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <atomic>
#include <cstdint>

class DragonDrainState
{
public:
    DragonDrainState() = default;
    DragonDrainState(const DragonDrainState &) = delete;
    DragonDrainState &operator=(const DragonDrainState &) = delete;

    void notifyDecodeFinished()
    {
        m_word.fetch_or(kDecodeFinished, std::memory_order_acq_rel);
    }

    void startNewEpoch()
    {
        auto current = m_word.load(std::memory_order_acquire);
        while (true) {
            const auto next = (current & ~kFlagsMask) + kEpochStep;
            if (m_word.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
        }
    }

    [[nodiscard]] std::uint64_t epoch() const
    {
        return m_word.load(std::memory_order_acquire) >> kEpochShift;
    }

    [[nodiscard]] bool decodeFinished() const
    {
        return m_word.load(std::memory_order_acquire) & kDecodeFinished;
    }

    [[nodiscard]] bool tryClaimDrain()
    {
        auto current = m_word.load(std::memory_order_acquire);
        while (current & kDecodeFinished && !(current & kDrainInitiated) && !(current & kDrainEmitted)) {
            if (m_word.compare_exchange_weak(current, current | kDrainInitiated, std::memory_order_acq_rel, std::memory_order_acquire)) {
                m_claimedEpoch = current >> kEpochShift;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t claimedEpoch() const
    {
        return m_claimedEpoch;
    }

    [[nodiscard]] bool claimIsCurrent() const
    {
        return claimIsCurrent(m_claimedEpoch);
    }

    [[nodiscard]] bool claimIsCurrent(std::uint64_t epoch) const
    {
        const auto current = m_word.load(std::memory_order_acquire);
        return (current >> kEpochShift) == epoch && (current & kDrainInitiated);
    }

    void consumeEmission()
    {
        auto current = m_word.load(std::memory_order_acquire);
        while (current & kDrainInitiated) {
            if (m_word.compare_exchange_weak(current, (current & ~kDrainInitiated) | kDrainEmitted, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
        }
    }

private:
    static constexpr std::uint64_t kDecodeFinished = 1;
    static constexpr std::uint64_t kDrainInitiated = 1 << 1;
    static constexpr std::uint64_t kDrainEmitted = 1 << 2;
    static constexpr std::uint64_t kFlagsMask = kDecodeFinished | kDrainInitiated | kDrainEmitted;
    static constexpr std::uint64_t kEpochShift = 3;
    static constexpr std::uint64_t kEpochStep = 1 << kEpochShift;

    std::atomic<std::uint64_t> m_word{0};
    std::uint64_t m_claimedEpoch = 0;
};
