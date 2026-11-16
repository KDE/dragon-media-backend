/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include "producerstate.h"

#include <QtGlobal>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>

/**
 * Stream-progress state machine for one playback stream.
 *
 * Owns everything that answers "where is the stream?": the producer epoch and
 * sample counter, the boundary ring that drives track starts and format
 * stalls, the end-of-stream claim state, the read/written/position counters,
 * the sample format, the producer gate, and the underrun counter. The
 * sample-domain work (fade envelopes, rendering) stays in the owning Voice,
 * which consults this clock from its render path.
 *
 * Threading contract:
 *  - producer-side methods run on the decode thread (gated by ProducerGate: Open,
 *    SwitchArmed, Closed),
 *  - staging methods run on the Qt main thread (reserveEpoch, armSwitch, closeGate, reopenIfIdle),
 *  - the consume/note/advance methods run on the audio callback thread,
 *  - beginStream(epoch) is the audio-thread authority that publishes the stream
 *    staged with that epoch and reopens the gate,
 *  - the atomics are the cross-thread staging points, documented per use.
 */
class DRAGONMEDIABACKEND_EXPORT StreamClock
{
public:
    // Invoked on the audio thread
    struct EventHandlers {
        std::function<void(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration)> trackStarted;
        std::function<void(int sampleRate, int channels)> formatChangeRequired;
    };

    StreamClock();
    ~StreamClock();

    StreamClock(const StreamClock &) = delete;
    StreamClock &operator=(const StreamClock &) = delete;
    StreamClock(StreamClock &&) = delete;
    StreamClock &operator=(StreamClock &&) = delete;

    void setEventHandlers(EventHandlers handlers);

    struct StreamBoundary {
        std::uint64_t epoch = 0; // stream epoch the entry was recorded in
        std::uint64_t streamSample = 0; // absolute sample index the boundary applies after
        std::uint32_t trackId = 0;
        int sampleRate = 0;
        int channels = 0;
        std::optional<std::chrono::milliseconds> duration = std::nullopt;
    };

    // Called from the decode thread between two sample chunks. Returns false when the
    // boundary ring is full (logged; extremely unlikely, boundaries are rare
    // and always drained by the callback).
    //
    // epochTag must be the stream epoch the producer observed before the
    // write/promotion this boundary belongs to, mirroring samplesProduced()'s
    // epochTag.
    bool boundaryTrackStart(std::uint32_t trackId, int sampleRate, int channels, std::optional<std::chrono::milliseconds> duration, std::uint64_t epochTag);

    // Arm the end-of-stream marker for the stream identified by epochTag.
    bool boundaryEndOfStream(std::uint64_t epochTag);

    bool retractEndOfStream();

    void samplesProduced(std::uint64_t epochTag, std::size_t count);

    [[nodiscard]] std::uint64_t streamEpoch() const
    {
        return m_producerState.epoch();
    }

    enum class ProducerGate : std::uint8_t {
        Open, // producers may write; counted
        SwitchArmed, // a stream-switch request is staged; the audio thread publishes it
        Closed, // gate held closed with no pending audio-thread action (hard hold / teardown)
    };

    // Atomically bump the epoch and return the new value. Called exactly once
    // per staged stream switch; the request carries the returned epoch and
    // beginStream() publishes the stream under it.
    [[nodiscard]] std::uint64_t reserveEpoch();
    // Gate the producers because a switch request is about to be staged.
    // Open -> SwitchArmed; a Closed gate is taken over as well (the caller
    // staging after closeGate(), e.g. the seek path).
    void armSwitch();
    void closeGate();
    bool reopenIfIdle();
    [[nodiscard]] bool isQueueReady() const;
    [[nodiscard]] bool isSwitchArmed() const;

    // Stage the new position offset (reported until the audio callback has
    // consumed the switch request that resets the position).
    void setPositionOffset(std::chrono::milliseconds offset);
    [[nodiscard]] std::chrono::milliseconds positionOffset() const;

    void setFormat(int sampleRate, int channels);
    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;
    [[nodiscard]] int currentSampleRate() const
    {
        return m_sampleRate.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int currentChannels() const
    {
        return m_channels.load(std::memory_order_relaxed);
    }

    // Publish a new stream under the epoch minted at staging time
    void beginStream(std::uint64_t epoch);

    enum class BoundaryAdvance {
        NoneCrossed,
        Crossed,
        FormatStall,
    };
    // Walk the boundary ring: discard stale-epoch entries, clamp the read
    // budget to the next boundary in the current epoch (beforeRead), stall on
    // a format change, and fire trackStarted when a boundary is crossed after
    // a read. Runs on the audio callback thread.
    BoundaryAdvance advanceBoundaries(bool afterRead, size_t *budget);

    void noteSamplesRead(std::size_t count);
    void noteSamplesRendered(std::size_t count);

    [[nodiscard]] bool endOfStreamPending() const;

    [[nodiscard]] bool endOfStreamArmed() const
    {
        return m_eosState.load(std::memory_order_acquire) != static_cast<std::int8_t>(EosState::None);
    }

    [[nodiscard]] bool tryClaimDrained();

    [[nodiscard]] bool takeDrainedEmission();

    [[nodiscard]] qint64 totalSamplesWritten() const;
    [[nodiscard]] qint64 streamSamplesRead() const
    {
        return static_cast<qint64>(m_streamSamplesRead.load(std::memory_order_relaxed));
    }
    [[nodiscard]] qint64 producerSamples() const
    {
        return static_cast<qint64>(m_producerState.samples());
    }
    void resetPositionTracking();
    // Zero the written counter only; part of the quiesced voice resets.
    void resetWrittenCounter();

    [[nodiscard]] int underrunCount() const
    {
        return m_underrunCount.load(std::memory_order_relaxed);
    }
    // Count one starvation quantum; the owning Voice decides when a silent
    // render actually is an underrun (it owns the fade state that suppresses
    // the count during fades).
    void noteUnderrun();

    // Full teardown (quiesced: audio callback stopped). Drains the boundary
    // ring, clears EOS state, position offset, and format-stall flag, and
    // reopens the gate while preserving epoch and sample counters.
    void reset();

private:
    class BoundaryRing
    {
    public:
        static constexpr std::size_t kCapacity = 8;

        bool push(const StreamBoundary &boundary)
        {
            std::scoped_lock lock(m_pushMutex);
            const auto head = m_head.load(std::memory_order_relaxed);
            const auto tail = m_tail.load(std::memory_order_acquire);
            if (head - tail >= kCapacity) {
                return false;
            }
            m_storage[head & kMask] = boundary;
            m_head.store(head + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] const StreamBoundary *peek() const
        {
            return peekAt(0);
        }

        // The boundary `offset` entries ahead of the tail (0 == peek()).
        [[nodiscard]] const StreamBoundary *peekAt(std::size_t offset) const
        {
            const auto tail = m_tail.load(std::memory_order_relaxed);
            if (m_head.load(std::memory_order_acquire) - tail <= offset) {
                return nullptr;
            }
            return &m_storage[(tail + offset) & kMask];
        }

        void pop()
        {
            m_tail.fetch_add(1, std::memory_order_release);
        }

    private:
        static constexpr std::size_t kMask = kCapacity - 1;

        mutable std::mutex m_pushMutex;
        std::array<StreamBoundary, kCapacity> m_storage{};
        std::atomic<std::uint64_t> m_head{0};
        std::atomic<std::uint64_t> m_tail{0};
    };

    enum class EosState : std::int8_t {
        None,
        Pending,
        Claimed,
        Emitted,
    };

    EventHandlers m_handlers;

    // Producer side (decode thread)
    alignas(64) BoundaryRing m_boundaries;
    ProducerState m_producerState{1};

    // Audio callback side (audio thread)
    alignas(64) std::atomic<std::uint64_t> m_streamSamplesRead{0};
    std::atomic<qint64> m_totalSamplesWritten{0};

    // Main-thread staging side
    alignas(64) std::atomic<std::chrono::milliseconds::rep> m_positionOffset{0};
    std::atomic<int> m_sampleRate{0};
    std::atomic<int> m_channels{0};
    std::atomic<ProducerGate> m_gate{ProducerGate::Open};

    alignas(64) std::atomic<std::int8_t> m_eosState{static_cast<std::int8_t>(EosState::None)};
    std::atomic<std::uint64_t> m_eosSample{0};
    std::atomic<bool> m_formatChangePending{false};
    std::atomic<int> m_underrunCount{0};
};
