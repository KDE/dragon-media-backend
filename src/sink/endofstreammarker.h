/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "producerstate.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>

/**
 * Thread-safe end-of-stream claim state for one stream epoch.
 *
 * Encapsulates a 64-bit atomic word containing:
 * - 20 bits epoch (bits 44..63), the stream epoch the marker belongs to
 * - 2 bits phase (bits 42..43): None -> Pending -> Claimed -> Emitted
 * - 42 bits sample (bits 0..41), the producer sample count at which EOS applies
 *
 * Every transition is a CAS on the whole word, so a marker from a superseded
 * epoch can never be claimed or emitted in the current one.
 *
 * Methods that compare against the stream's read position take it as a
 * callable rather than a value: the read counter must be loaded *after* the
 * acquire load of the marker word, which synchronizes with the release store
 * in reset() that follows the counter reset when a new stream is published.
 */
class EndOfStreamMarker
{
public:
    explicit EndOfStreamMarker(std::uint64_t initialEpoch = 1) noexcept
        : m_word(pack(initialEpoch, Phase::None, 0))
    {
    }

    bool arm(std::uint64_t epoch, std::uint64_t sample) noexcept
    {
        auto expected = pack(epoch, Phase::None, 0);
        const auto desired = pack(epoch, Phase::Pending, std::min(sample, kSampleMask));
        return m_word.compare_exchange_strong(expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    bool retract(std::uint64_t epoch) noexcept
    {
        auto current = m_word.load(std::memory_order_acquire);
        while (unpackEpoch(current) == epoch && unpackPhase(current) == Phase::Pending) {
            const auto desired = pack(epoch, Phase::None, 0);
            if (m_word.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    template<std::invocable SamplesReadFn>
    [[nodiscard]] bool reached(std::uint64_t epoch, SamplesReadFn samplesRead) const
    {
        const auto word = m_word.load(std::memory_order_acquire);
        return unpackEpoch(word) == epoch && unpackPhase(word) == Phase::Pending && samplesRead() >= unpackSample(word);
    }

    [[nodiscard]] bool isArmed(std::uint64_t epoch) const noexcept
    {
        const auto word = m_word.load(std::memory_order_acquire);
        return unpackEpoch(word) == epoch && unpackPhase(word) != Phase::None;
    }

    template<std::invocable EpochFn, std::invocable SamplesReadFn>
    [[nodiscard]] bool tryClaim(EpochFn currentEpoch, SamplesReadFn samplesRead)
    {
        const std::uint64_t epoch = currentEpoch();
        auto current = m_word.load(std::memory_order_acquire);
        if (unpackEpoch(current) != epoch || unpackPhase(current) != Phase::Pending) {
            return false;
        }
        if (samplesRead() < unpackSample(current)) {
            return false;
        }
        if (currentEpoch() != epoch) {
            return false;
        }
        const auto desired = pack(epoch, Phase::Claimed, unpackSample(current));
        return m_word.compare_exchange_strong(current, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    [[nodiscard]] bool takeEmission(std::uint64_t epoch) noexcept
    {
        auto current = m_word.load(std::memory_order_acquire);
        while (unpackEpoch(current) == epoch && unpackPhase(current) == Phase::Claimed) {
            const auto desired = pack(epoch, Phase::Emitted, unpackSample(current));
            if (m_word.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void reset(std::uint64_t epoch) noexcept
    {
        m_word.store(pack(epoch, Phase::None, 0), std::memory_order_release);
    }

private:
    enum class Phase : std::uint8_t {
        None = 0,
        Pending = 1,
        Claimed = 2,
        Emitted = 3,
    };

    static constexpr std::uint64_t kSampleBits = 42;
    static constexpr std::uint64_t kSampleMask = (std::uint64_t{1} << kSampleBits) - 1;
    static constexpr std::uint64_t kPhaseShift = kSampleBits;
    static constexpr std::uint64_t kPhaseMask = std::uint64_t{0x3};
    static constexpr std::uint64_t kEpochShift = kSampleBits + 2;
    static constexpr std::uint64_t kEpochMask = ProducerState::kEpochMask;
    static_assert(kEpochShift + ProducerState::kEpochBits == 64, "EOS word layout must fill exactly 64 bits");

    static constexpr std::uint64_t pack(std::uint64_t epoch, Phase phase, std::uint64_t sample) noexcept
    {
        return ((epoch & kEpochMask) << kEpochShift) | ((static_cast<std::uint64_t>(phase) & kPhaseMask) << kPhaseShift) | (sample & kSampleMask);
    }

    static constexpr std::uint64_t unpackEpoch(std::uint64_t word) noexcept
    {
        return (word >> kEpochShift) & kEpochMask;
    }

    static constexpr Phase unpackPhase(std::uint64_t word) noexcept
    {
        return static_cast<Phase>((word >> kPhaseShift) & kPhaseMask);
    }

    static constexpr std::uint64_t unpackSample(std::uint64_t word) noexcept
    {
        return word & kSampleMask;
    }

    std::atomic<std::uint64_t> m_word;
};
