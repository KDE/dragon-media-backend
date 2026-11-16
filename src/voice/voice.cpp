/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "voice.h"
#include "fft/pcmblock.h"
#include "player/pipe.h"
#include "sink/dragonmediabackend_audio_logging.h"

#include <algorithm>
#include <ranges>
#include <thread>

using namespace std::chrono_literals;

Voice::Voice(Pipe<float> *pipe, std::uint64_t generation, Notifiers notifiers)
    : AudioSinkStreamRenderer()
    , m_channel(pipe)
    , m_generation(generation)
    , m_notifiers(std::move(notifiers))
{
    // The link starts dormant: no outgoing voice is holding or fading. It
    // becomes live when this voice is the incoming of an armed crossfade
    // (demoteToFading resets it on the outgoing voice).
    m_crossfadeLink.incomingStarted.store(true, std::memory_order_relaxed);
    m_crossfadeLink.outgoingActive.store(false, std::memory_order_relaxed);

    StreamClock::EventHandlers handlers;
    handlers.trackStarted = [this](std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration) {
        if (m_notifiers.trackStarted && m_role.load(std::memory_order_relaxed) == Role::Primary) {
            m_notifiers.trackStarted(trackId, duration);
        }
    };
    handlers.formatChangeRequired = [this](int sampleRate, int channels) {
        if (m_notifiers.formatChangeRequired && m_role.load(std::memory_order_relaxed) == Role::Primary) {
            m_notifiers.formatChangeRequired(sampleRate, channels);
        }
    };
    m_clock.setEventHandlers(std::move(handlers));
}

Voice::~Voice()
{
    // Mandatory destruction order: detach the FFT tap (waiting for in-flight
    // writers) first; the stream (declared last) is then destroyed —
    // quiescing its callback — before the channel and the clock. If callback
    // quiescence failed inside the backend, the stream and everything behind
    // it are leaked into the backend's zombie safeguard together with this
    // whole voice (the pipeline never frees the Voice object in that case).
    setFftPipe(nullptr);

    // Close the backend stream and quiesce its audio callback while this
    // object is still complete: the stream's own destructor must never be
    // the first close (a base-class virtual call there is a pure-virtual
    // abort for backends with defaulted destructors).
    if (m_stream) {
        std::ignore = m_stream->close();
    }

    // A fading voice must not leave its link claiming an active outgoing
    // (it would suppress primary underruns forever).
    if (m_role.load(std::memory_order_relaxed) == Role::Fading) {
        if (auto *link = m_demotedLink.load(std::memory_order_relaxed)) {
            link->outgoingActive.store(false, std::memory_order_release);
        }
    }
}

std::uint64_t Voice::publishInitialStream(std::chrono::milliseconds positionOffset)
{
    const auto epoch = m_clock.reserveEpoch();
    m_clock.armSwitch();
    m_clock.setPositionOffset(positionOffset);
    m_clock.beginStream(epoch);
    return epoch;
}

void Voice::setPendingFadeIn(std::chrono::milliseconds duration) noexcept
{
    m_pendingFadeIn = std::max<std::chrono::milliseconds>(0ms, duration);
}

std::chrono::milliseconds Voice::pendingFadeIn() const noexcept
{
    return m_pendingFadeIn;
}

void Voice::setFormat(int sampleRate, int channels)
{
    m_clock.setFormat(sampleRate, channels);
    m_clock.resetWrittenCounter();
    m_channel.onFormatReopen(sampleRate);
}

AudioSinkStream *Voice::stream() const noexcept
{
    return m_stream.get();
}

void Voice::configureFadeIn(std::chrono::milliseconds fadeIn)
{
    m_channel.configureFadeIn(fadeIn, m_clock.currentSampleRate());
}

void Voice::setStream(std::unique_ptr<AudioSinkStream> stream)
{
    m_stream = std::move(stream);
    m_pendingFadeIn = std::chrono::milliseconds{0};
}

void Voice::installStream(std::unique_ptr<AudioSinkStream> stream, std::chrono::milliseconds fadeIn)
{
    configureFadeIn(fadeIn);
    setStream(std::move(stream));
}

bool Voice::closeStream()
{
    if (!m_stream) {
        return true;
    }
    if (!m_stream->close()) {
        // Zombie safeguard: the backend leaked its native stream and its
        // callback may still be running. Release ownership here too so the
        // voice's destructor cannot free the stream object under it; the
        // pipeline leaks this whole voice instead.
        std::ignore = m_stream.release();
        return false;
    }
    m_stream.reset();
    return true;
}

std::uint64_t Voice::stageFlush(std::chrono::milliseconds fadeIn)
{
    const auto epoch = m_clock.reserveEpoch();

    // Even/odd seqlock protocol (single writer on Qt main thread, single
    // reader on audio thread): bump to odd before writing the payload, then
    // arm the switch and bump to even so the reader either sees a consistent
    // pair or retries without a C++ data race.
    const auto seq = m_flushSeq.load(std::memory_order_relaxed);
    m_flushSeq.store(seq + 1, std::memory_order_release);
    m_flushEpoch.store(epoch, std::memory_order_release);
    m_flushFadeInMs.store(fadeIn.count(), std::memory_order_release);
    m_clock.armSwitch();
    m_flushSeq.store(seq + 2, std::memory_order_release);
    return epoch;
}

std::uint64_t Voice::runFlushQuiesced(std::chrono::milliseconds fadeIn)
{
    const auto epoch = stageFlush(fadeIn);
    consumePendingFlush();
    return epoch;
}

bool Voice::isFlushPending() const noexcept
{
    return m_flushSeq.load(std::memory_order_acquire) != m_flushConsumed.load(std::memory_order_acquire);
}

void Voice::resetForTeardown()
{
    // Discard any flush staged for a callback that will never run, and the
    // pending fade-in of a stream that was never opened.
    m_flushConsumed.store(m_flushSeq.load(std::memory_order_acquire), std::memory_order_release);
    m_pendingFadeIn = std::chrono::milliseconds{0};
    m_channel.resetFadeIn();
    m_clock.resetWrittenCounter();
    m_clock.reset();
}

void Voice::demoteToFading(CrossfadeLink *link, std::chrono::milliseconds fadeOut, bool incomingHasFadeIn)
{
    if (link == nullptr) {
        return;
    }

    // (Re)publish the link as live: the incoming track has not produced a
    // sample yet, and the outgoing voice is holding or fading from now on.
    link->incomingStarted.store(false, std::memory_order_release);
    link->outgoingActive.store(true, std::memory_order_release);

    FadeOutRequest request;
    request.duration = std::max<std::chrono::milliseconds>(0ms, fadeOut);
    request.holdMax = kSlowLoadHoldMax;
    request.initialGain = m_channel.currentGainForCrossfade();
    request.incomingHasFadeIn = incomingHasFadeIn;
    m_fadeOutCommand = request;

    m_demotedLink.store(link, std::memory_order_release);
    m_fadeOutRequest.store(&m_fadeOutCommand, std::memory_order_release);
    m_role.store(Role::Fading, std::memory_order_release);
}

std::span<const float> Voice::render(std::size_t maxSamples, std::chrono::microseconds estimatedPts)
{
    const Role role = m_role.load(std::memory_order_acquire);
    if (role == Role::Primary) {
        consumePendingFlush();
    } else {
        consumeFadeOutRequest();
    }

    const int channels = std::max(1, m_clock.currentChannels());
    const auto ch = static_cast<std::size_t>(channels);

    std::size_t budget = (maxSamples / ch) * ch;
    if (budget == 0) {
        return {};
    }
    if (m_clock.advanceBoundaries(false, &budget) == StreamClock::BoundaryAdvance::FormatStall || budget == 0) {
        return {};
    }

    const bool holdForSlowLoad = (role == Role::Fading) && crossfadeHoldForSlowLoad();
    if (role == Role::Fading) {
        budget = m_channel.fadeOutBudgetSamples(budget, channels, holdForSlowLoad);
        if (budget == 0) {
            checkFadingFinished();
            return {};
        }
    }
    ensureRenderStorage(budget);

    const std::size_t read = m_channel.pull(renderSpan(budget), budget);
    if (read > 0) {
        m_clock.noteSamplesRead(read);
        if (m_clock.advanceBoundaries(true, nullptr) != StreamClock::BoundaryAdvance::Crossed) {
            m_clock.noteSamplesRendered(read);
        }
        if (role == Role::Primary) {
            m_channel.applyFadeIn(renderSpan(read), read, channels);
            // First non-empty render: the outgoing slow-load hold may end.
            m_crossfadeLink.incomingStarted.store(true, std::memory_order_release);
            forwardToFft(renderSpan(read).first(read), estimatedPts);
        } else {
            m_channel.applyFadeOut(renderSpan(read), read, channels, holdForSlowLoad);
        }
    } else if (role == Role::Fading) {
        if (!m_channel.producerDone()) {
            // The hold and fade budgets advance on real unpaused callback
            // quanta even when the retiring pipe is starved.
            m_channel.advanceFadeOutOverSilence(budget, channels, holdForSlowLoad);
        }
    } else {
        noteUnderrunIfUnexpected();
    }

    if (role == Role::Fading) {
        checkFadingFinished();
    }

    return std::span<const float>{m_renderBuffer}.first(read);
}

bool Voice::endOfStreamPending() const
{
    return m_clock.endOfStreamPending();
}

bool Voice::endOfStreamArmed() const
{
    return m_clock.endOfStreamArmed();
}

bool Voice::tryClaimDrained()
{
    return m_clock.tryClaimDrained();
}

bool Voice::takeDrainedEmission()
{
    return m_clock.takeDrainedEmission();
}

void Voice::setFftPipe(Pipe<PcmBlock> *pipe)
{
    m_fftPipe.store(pipe, std::memory_order_seq_cst);
    if (pipe == nullptr) {
        while (m_fftWriters.load(std::memory_order_seq_cst) > 0) {
            std::this_thread::yield();
        }
    }
}

void Voice::preAllocate(std::size_t maxSamples)
{
    constexpr std::size_t kMinAllocation = 16384;
    const std::size_t allocation = std::max(maxSamples, kMinAllocation);
    m_renderBuffer.resize(allocation);
}

void Voice::consumePendingFlush()
{
    const std::uint64_t consumed = m_flushConsumed.load(std::memory_order_relaxed);
    std::uint64_t seq = m_flushSeq.load(std::memory_order_acquire);
    if (seq == consumed) {
        return;
    }

    // Read the command under the single-writer even/odd seqlock; a write in
    // progress (odd seq) or superseded flush (seq changed) is retried.
    for (int attempts = 0; attempts < 4; ++attempts) {
        if ((seq & 1u) != 0) {
            seq = m_flushSeq.load(std::memory_order_acquire);
            continue;
        }
        if (seq == consumed) {
            return;
        }
        const std::uint64_t epoch = m_flushEpoch.load(std::memory_order_acquire);
        const std::int64_t fadeInMs = m_flushFadeInMs.load(std::memory_order_acquire);
        const std::uint64_t seqAfter = m_flushSeq.load(std::memory_order_acquire);
        if (seqAfter != seq) {
            seq = seqAfter;
            continue;
        }
        consumeFlushBody(epoch, std::chrono::milliseconds{fadeInMs});
        m_flushConsumed.store(seq, std::memory_order_release);
        return;
    }
}

void Voice::consumeFlushBody(std::uint64_t epoch, std::chrono::milliseconds fadeIn)
{
    m_channel.drainPipe();
    m_channel.resetFadeIn();
    if (fadeIn > 0ms) {
        m_channel.configureFadeIn(fadeIn, m_clock.currentSampleRate());
    }

    // The staged position offset survives a flush (it encodes the seek
    // target); only publishInitialStream resets it (new track from zero).
    m_clock.beginStream(epoch);
}

void Voice::consumeFadeOutRequest()
{
    const auto *request = m_fadeOutRequest.exchange(nullptr, std::memory_order_acq_rel);
    if (request == nullptr) {
        return;
    }

    FadeOutEnvelope::Config config;
    config.duration = request->duration;
    config.holdMax = request->holdMax;
    config.initialGain = request->initialGain;
    config.incomingHasFadeIn = request->incomingHasFadeIn;
    config.sampleRate = m_clock.currentSampleRate();
    m_channel.configureFadeOut(config);
}

bool Voice::crossfadeHoldForSlowLoad() const noexcept
{
    const auto *link = m_demotedLink.load(std::memory_order_acquire);
    return link != nullptr && !link->incomingStarted.load(std::memory_order_acquire);
}

void Voice::noteUnderrunIfUnexpected()
{
    const bool inInitialFadeLoad = m_channel.isWaitingForFirstSample() && m_clock.streamSamplesRead() == 0;
    // The primary (incoming) voice owns the link; an active outgoing voice
    // suppresses loading-starvation underruns while it holds or fades.
    const bool outgoingStillActive = m_crossfadeLink.outgoingActive.load(std::memory_order_acquire);
    if (!m_clock.endOfStreamPending() && !outgoingStillActive && !inInitialFadeLoad) {
        m_clock.noteUnderrun();
    }
}

void Voice::checkFadingFinished()
{
    if (m_fadingFinishedPosted.load(std::memory_order_acquire)) {
        return;
    }

    const bool pipeDrained = m_channel.producerDone() && m_channel.pipe() != nullptr && m_channel.pipe()->consumer().ready() == 0;
    if (!m_channel.isFadeOutActive() || pipeDrained) {
        if (auto *link = m_demotedLink.load(std::memory_order_relaxed)) {
            link->outgoingActive.store(false, std::memory_order_release);
        }
        if (m_fadingFinishedPosted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (m_notifiers.fadingFinished) {
            m_notifiers.fadingFinished();
        }
    }
}

void Voice::forwardToFft(std::span<const float> pcm, std::chrono::microseconds pts)
{
    struct WriterGuard {
        std::atomic<int> &counter;
        ~WriterGuard()
        {
            counter.fetch_sub(1, std::memory_order_seq_cst);
        }
    };

    m_fftWriters.fetch_add(1, std::memory_order_seq_cst);
    WriterGuard guard{m_fftWriters};

    auto *fftPipe = m_fftPipe.load(std::memory_order_seq_cst);
    if (fftPipe == nullptr || pcm.empty()) {
        return;
    }

    const int sampleRate = m_clock.currentSampleRate();
    const int channels = m_clock.currentChannels();

    auto chunks = pcm | std::views::chunk(PcmBlock::MAX_SAMPLES);
    fftPipe->producer().writeSomeWith(std::ranges::size(chunks), [&](std::span<PcmBlock> fb1, std::span<PcmBlock> fb2) {
        auto chunkIt = chunks.begin();
        const auto chunkEnd = chunks.end();
        auto fillBlocks = [&](std::span<PcmBlock> dst) {
            for (auto &block : dst) {
                if (chunkIt == chunkEnd) {
                    break;
                }
                const auto chunk = *chunkIt;
                block.count = chunk.size();
                block.pts = pts;
                block.sampleRate = sampleRate;
                block.channels = channels;
                std::ranges::copy(chunk, block.samples.begin());
                ++chunkIt;
            }
        };
        fillBlocks(fb1);
        fillBlocks(fb2);
    });
}

void Voice::ensureRenderStorage(std::size_t budget)
{
    if (m_renderBuffer.size() < budget) {
        m_renderBuffer.resize(budget);
    }
}
