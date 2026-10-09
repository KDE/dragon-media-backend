/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <atomic>
#include <cstdint>

/**
 * Thread-safe packed stream epoch and producer sample counter.
 *
 * Encapsulates a 64-bit atomic word containing:
 * - 20 bits epoch (high bits, 1 .. 1,048,575)
 * - 44 bits sample count (low bits, up to ~17.59 trillion samples, ~1.45 years of 192kHz continuous audio)
 */
class DragonProducerState
{
public:
    static constexpr std::uint64_t kEpochBits = 20;
    static constexpr std::uint64_t kSamplesBits = 64 - kEpochBits; // 44 bits
    static constexpr std::uint64_t kSamplesMask = (std::uint64_t{1} << kSamplesBits) - 1;
    static constexpr std::uint64_t kEpochMask = (std::uint64_t{1} << kEpochBits) - 1;

    explicit DragonProducerState(std::uint64_t initialEpoch = 1) noexcept
        : m_state(pack(initialEpoch, 0))
    {
    }

    [[nodiscard]] std::uint64_t epoch(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return unpackEpoch(m_state.load(order));
    }

    [[nodiscard]] std::uint64_t samples(std::memory_order order = std::memory_order_relaxed) const noexcept
    {
        return unpackSamples(m_state.load(order));
    }

    std::uint64_t bumpEpoch() noexcept
    {
        auto current = m_state.load(std::memory_order_relaxed);
        while (true) {
            const auto next = pack(nextEpoch(unpackEpoch(current)), 0);
            if (m_state.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return unpackEpoch(next);
            }
        }
    }

    bool addSamples(std::uint64_t epochTag, std::size_t count) noexcept
    {
        if (count == 0) {
            return true;
        }
        auto current = m_state.load(std::memory_order_relaxed);
        while (true) {
            if (unpackEpoch(current) != epochTag) {
                return false;
            }
            const auto newSamples = unpackSamples(current) + count;
            const auto next = pack(epochTag, newSamples);
            if (m_state.compare_exchange_weak(current, next, std::memory_order_release, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    void reset(std::uint64_t epoch = 1, std::uint64_t samples = 0) noexcept
    {
        m_state.store(pack(epoch, samples), std::memory_order_release);
    }

private:
    static constexpr std::uint64_t pack(std::uint64_t epoch, std::uint64_t samples) noexcept
    {
        return ((epoch & kEpochMask) << kSamplesBits) | (samples & kSamplesMask);
    }

    static constexpr std::uint64_t unpackEpoch(std::uint64_t state) noexcept
    {
        return (state >> kSamplesBits) & kEpochMask;
    }

    static constexpr std::uint64_t unpackSamples(std::uint64_t state) noexcept
    {
        return state & kSamplesMask;
    }

    static constexpr std::uint64_t nextEpoch(std::uint64_t epoch) noexcept
    {
        const auto next = (epoch + 1) & kEpochMask;
        return next == 0 ? 1 : next;
    }

    std::atomic<std::uint64_t> m_state;
};
