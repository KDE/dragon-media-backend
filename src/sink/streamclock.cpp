/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "streamclock.h"

#include "dragonmediabackend_audio_logging.h"

#include <algorithm>

StreamClock::StreamClock() = default;

StreamClock::~StreamClock() = default;

void StreamClock::setEventHandlers(EventHandlers handlers)
{
    m_handlers = std::move(handlers);
}

bool StreamClock::boundaryTrackStart(std::uint32_t trackId,
                                     int sampleRate,
                                     int channels,
                                     std::optional<std::chrono::milliseconds> duration,
                                     std::uint64_t epochTag)
{
    StreamBoundary boundary;
    // Stamped with the epoch the producer observed before the write/promotion
    // this boundary belongs to (not the current one): if a flush superseded
    // the stream in between, the consumer's epoch check discards the boundary
    // instead of letting it fire in the new stream (a ghost boundary).
    boundary.epoch = epochTag;
    boundary.streamSample = m_producerState.samples();
    boundary.trackId = trackId;
    boundary.sampleRate = sampleRate;
    boundary.channels = channels;
    boundary.duration = duration;

    if (!m_boundaries.push(boundary)) {
        qCWarning(dragonMediaBackendAudio) << "boundary ring full; dropping track boundary (seek/flush the stream first)";
        return false;
    }
    return true;
}

bool StreamClock::boundaryEndOfStream(std::uint64_t epochTag)
{
    auto expected = static_cast<std::int8_t>(EosState::None);
    if (m_eosState.load(std::memory_order_relaxed) != expected) {
        return false;
    }
    if (m_producerState.epoch(std::memory_order_acquire) != epochTag) {
        // The stream was superseded before the EOS could be armed.
        return false;
    }
    m_eosSample.store(m_producerState.samples(), std::memory_order_release);
    if (!m_eosState.compare_exchange_strong(expected, static_cast<std::int8_t>(EosState::Pending), std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    if (m_producerState.epoch(std::memory_order_acquire) != epochTag) {
        // A stream switch published a new epoch between the sample read and
        // the CAS: the EOS would otherwise apply to the new stream with a
        // bogus (or wrapped) sample count. Retract instead.
        expected = static_cast<std::int8_t>(EosState::Pending);
        std::ignore =
            m_eosState.compare_exchange_strong(expected, static_cast<std::int8_t>(EosState::None), std::memory_order_acq_rel, std::memory_order_acquire);
        return false;
    }
    return true;
}

bool StreamClock::retractEndOfStream()
{
    auto expected = static_cast<std::int8_t>(EosState::Pending);
    return m_eosState.compare_exchange_strong(expected, static_cast<std::int8_t>(EosState::None), std::memory_order_acq_rel, std::memory_order_acquire);
}

void StreamClock::samplesProduced(std::uint64_t epochTag, std::size_t count)
{
    m_producerState.addSamples(epochTag, count);
}

std::uint64_t StreamClock::reserveEpoch()
{
    return m_producerState.bumpEpoch();
}

void StreamClock::armSwitch()
{
    ProducerGate expected = ProducerGate::Open;
    if (m_gate.compare_exchange_strong(expected, ProducerGate::SwitchArmed, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (expected == ProducerGate::Closed) {
        // The caller staging after closeGate() (the seek path): take the
        // closed gate over, the staged request will publish/reopen it.
        expected = ProducerGate::Closed;
        if (!m_gate.compare_exchange_strong(expected, ProducerGate::SwitchArmed, std::memory_order_acq_rel, std::memory_order_acquire)) {
            qCWarning(dragonMediaBackendAudio) << "armSwitch(): unexpected gate state" << static_cast<int>(expected);
        }
        return;
    }
    // Already SwitchArmed: a request is staged; the caller supersedes it.
}

void StreamClock::closeGate()
{
    const auto prev = m_gate.exchange(ProducerGate::Closed, std::memory_order_acq_rel);
    if (prev == ProducerGate::SwitchArmed) {
        qCWarning(dragonMediaBackendAudio) << "closeGate(): a switch request was staged; closing anyway (teardown path)";
    }
}

bool StreamClock::reopenIfIdle()
{
    ProducerGate expected = ProducerGate::Closed;
    return m_gate.compare_exchange_strong(expected, ProducerGate::Open, std::memory_order_acq_rel, std::memory_order_acquire);
}

bool StreamClock::isQueueReady() const
{
    return m_gate.load(std::memory_order_acquire) == ProducerGate::Open;
}

bool StreamClock::isSwitchArmed() const
{
    return m_gate.load(std::memory_order_acquire) == ProducerGate::SwitchArmed;
}

void StreamClock::setPositionOffset(std::chrono::milliseconds offset)
{
    m_positionOffset.store(offset.count(), std::memory_order_relaxed);
}

std::chrono::milliseconds StreamClock::positionOffset() const
{
    return std::chrono::milliseconds{m_positionOffset.load(std::memory_order_relaxed)};
}

void StreamClock::setFormat(int sampleRate, int channels)
{
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
    m_channels.store(channels, std::memory_order_relaxed);
}

bool StreamClock::hasFormat(int sampleRate, int channels) const
{
    return m_sampleRate.load(std::memory_order_relaxed) == sampleRate && m_channels.load(std::memory_order_relaxed) == channels;
}

void StreamClock::beginStream(std::uint64_t epoch)
{
    if (m_producerState.epoch(std::memory_order_acquire) != epoch) {
        // Epochs reserved by refused or superseded stagings were skipped:
        // publishing rewinds the state to this request's epoch (with a zero
        // sample count) so producers stamping the request's tag are accepted.
        qCDebug(dragonMediaBackendAudio) << "beginStream: skipping reserved epoch" << m_producerState.epoch(std::memory_order_relaxed) << "back to" << epoch;
    }
    m_producerState.reset(epoch, 0);

    m_totalSamplesWritten.store(0, std::memory_order_relaxed);

    m_streamSamplesRead.store(0, std::memory_order_relaxed);
    while (m_boundaries.peek() != nullptr) {
        m_boundaries.pop();
    }

    // Release store so a consumer that later reads Pending/Claimed state
    // cannot observe a stale m_eosSample from the previous stream.
    m_eosSample.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
    m_eosState.store(static_cast<std::int8_t>(EosState::None), std::memory_order_release);
    m_formatChangePending.store(false, std::memory_order_relaxed);

    ProducerGate expected = ProducerGate::SwitchArmed;
    if (!m_gate.compare_exchange_strong(expected, ProducerGate::Open, std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (expected == ProducerGate::Open) {
            // Concurrently reopened (a staged request was cancelled); the
            // epoch-tag contract keeps producers from writing stale samples,
            // so publishing is still safe.
            qCDebug(dragonMediaBackendAudio) << "beginStream: gate already Open at publish (concurrent cancel)";
        } else {
            qCWarning(dragonMediaBackendAudio) << "beginStream: unexpected gate state" << static_cast<int>(expected) << "at publish; leaving gate unchanged";
        }
    }
}

StreamClock::BoundaryAdvance StreamClock::advanceBoundaries(bool afterRead, size_t *budget)
{
    BoundaryAdvance result = BoundaryAdvance::NoneCrossed;
    while (const StreamBoundary *boundary = m_boundaries.peek()) {
        if (boundary->epoch != streamEpoch()) {
            m_boundaries.pop();
            continue;
        }
        const std::uint64_t samplesRead = m_streamSamplesRead.load(std::memory_order_relaxed);
        if (samplesRead < boundary->streamSample) {
            // Clamp the pre-read budget at the earliest upcoming format-change
            // boundary so we don't feed wrong-format samples into the device.
            // Same-format boundaries are not clamped (a short quantum would
            // leave an audible gap at track start), though a format change
            // closely following one may render through the end of the current
            // quantum before stalling on the next.
            if (!afterRead && budget != nullptr) {
                for (std::size_t offset = 0;; ++offset) {
                    const StreamBoundary *upcoming = m_boundaries.peekAt(offset);
                    if (upcoming == nullptr) {
                        break;
                    }
                    if (upcoming->epoch != streamEpoch() || samplesRead >= upcoming->streamSample) {
                        continue; // stale entry, or already crossed
                    }
                    if (upcoming->sampleRate != currentSampleRate() || upcoming->channels != currentChannels()) {
                        *budget = std::min(*budget, static_cast<size_t>(upcoming->streamSample - samplesRead));
                        break;
                    }
                    // Same-format boundary: crossed mid-quantum, keep looking.
                }
            }
            break;
        }

        if (boundary->sampleRate != currentSampleRate() || boundary->channels != currentChannels()) {
            if (afterRead) {
                break;
            }
            if (!m_formatChangePending.exchange(true, std::memory_order_acq_rel)) {
                const int sampleRate = boundary->sampleRate;
                const int channels = boundary->channels;
                if (m_handlers.formatChangeRequired) {
                    m_handlers.formatChangeRequired(sampleRate, channels);
                }
            }
            return BoundaryAdvance::FormatStall;
        }

        const StreamBoundary boundaryCopy = *boundary;
        m_boundaries.pop();
        m_formatChangePending.store(false, std::memory_order_relaxed);
        m_positionOffset.store(0, std::memory_order_relaxed);
        m_totalSamplesWritten.store(afterRead ? static_cast<qint64>(samplesRead - boundaryCopy.streamSample) : static_cast<qint64>(0),
                                    std::memory_order_relaxed);
        const std::uint32_t trackId = boundaryCopy.trackId;
        const std::optional<std::chrono::milliseconds> duration = boundaryCopy.duration;
        if (m_handlers.trackStarted) {
            m_handlers.trackStarted(trackId, duration);
        }
        if (afterRead) {
            result = BoundaryAdvance::Crossed;
        }
    }
    return result;
}

void StreamClock::noteSamplesRead(std::size_t count)
{
    m_streamSamplesRead.fetch_add(count, std::memory_order_relaxed);
}

void StreamClock::noteSamplesRendered(std::size_t count)
{
    m_totalSamplesWritten.fetch_add(static_cast<qint64>(count), std::memory_order_relaxed);
}

bool StreamClock::endOfStreamPending() const
{
    return m_eosState.load(std::memory_order_acquire) == static_cast<std::int8_t>(EosState::Pending)
        && m_streamSamplesRead.load(std::memory_order_acquire) >= m_eosSample.load(std::memory_order_acquire);
}

bool StreamClock::tryClaimDrained()
{
    if (!endOfStreamPending()) {
        return false;
    }
    auto expected = static_cast<std::int8_t>(EosState::Pending);
    return m_eosState.compare_exchange_strong(expected, static_cast<std::int8_t>(EosState::Claimed), std::memory_order_acq_rel, std::memory_order_acquire);
}

bool StreamClock::takeDrainedEmission()
{
    auto expected = static_cast<std::int8_t>(EosState::Claimed);
    return m_eosState.compare_exchange_strong(expected, static_cast<std::int8_t>(EosState::Emitted), std::memory_order_acq_rel, std::memory_order_acquire);
}

qint64 StreamClock::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

void StreamClock::resetPositionTracking()
{
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    m_positionOffset.store(0, std::memory_order_relaxed);
}

void StreamClock::resetWrittenCounter()
{
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
}

void StreamClock::noteUnderrun()
{
    m_underrunCount.fetch_add(1, std::memory_order_relaxed);
}

void StreamClock::reset()
{
    // Clears EOS, boundaries, and position offset so a new stream can start,
    // while leaving the producer epoch and sample counters intact so
    // diagnostics still see the torn-down stream's final counts.
    while (m_boundaries.peek() != nullptr) {
        m_boundaries.pop();
    }
    m_eosSample.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
    m_eosState.store(static_cast<std::int8_t>(EosState::None), std::memory_order_release);
    m_positionOffset.store(0, std::memory_order_relaxed);
    m_formatChangePending.store(false, std::memory_order_relaxed);

    m_gate.store(ProducerGate::Open, std::memory_order_release);
}
