/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "mixer.h"
#include "fft/pcmblock.h"
#include "player/pipe.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <thread>

using namespace std::chrono_literals;

Mixer::Mixer(StreamClock &clock)
    : m_clock(clock)
{
}

Mixer::~Mixer() = default;

void Mixer::setAudioPipe(Pipe<float> *pipe)
{
    m_primary.setPipe(pipe);
}

void Mixer::setFftPipe(Pipe<PcmBlock> *pipe)
{
    m_fftPipe.store(pipe, std::memory_order_seq_cst);
    if (pipe == nullptr) {
        while (m_fftWriters.load(std::memory_order_seq_cst) > 0) {
            std::this_thread::yield();
        }
    }
}

std::uint64_t Mixer::flushStreams(Pipe<float> *primaryPipe, std::chrono::milliseconds fadeInDuration)
{
    const auto epoch = m_clock.reserveEpoch();
    m_clock.armSwitch();

    // Clear any staged request (flush-wins); an un-consumed crossfade's
    // primary pipe is carried over so the new track's pipe still gets installed.
    const bool wasPending = m_switch.isPending();
    const auto previous = m_switch.clear();
    Pipe<float> *carriedPrimaryPipe = primaryPipe;
    if (carriedPrimaryPipe == nullptr && wasPending && previous.kind == StreamSwitch::Kind::Crossfade) {
        carriedPrimaryPipe = previous.primaryPipe;
    }
    m_flushTail.clear();
    flushBody(carriedPrimaryPipe, epoch, fadeInDuration, m_clock.currentSampleRate());
    return epoch;
}

std::optional<std::uint64_t>
Mixer::armCrossfade(Pipe<float> *fadingPipe, Pipe<float> *newPrimaryPipe, std::chrono::milliseconds fadeOutDuration, std::chrono::milliseconds fadeInDuration)
{
    // Refuse early when a request is still un-consumed, without reserving an
    // epoch: the caller waits it out or supersedes it.
    if (m_switch.isPending()) {
        return std::nullopt;
    }
    // Always gate the new primary producer until render() latches the
    // handoff, installs both pipes atomically, and publishes the new stream
    // (which reopens the gate) on the audio thread. No fadeOutDuration == 0
    // special case: fade-in-only switches are gated exactly the same way.
    const auto epoch = m_clock.reserveEpoch();
    m_clock.armSwitch();

    StreamSwitch::Request request;
    request.kind = StreamSwitch::Kind::Crossfade;
    request.epoch = epoch;
    request.primaryPipe = newPrimaryPipe;
    request.fadingPipe = fadingPipe;
    request.fadeOut = fadeOutDuration;
    request.fadeIn = fadeInDuration;

    const bool staged = m_switch.stage(request);
    Q_ASSERT(staged);
    std::ignore = staged;
    return epoch;
}

std::uint64_t Mixer::stageFlush(Pipe<float> *primaryPipe, std::chrono::milliseconds fadeInDuration)
{
    const auto epoch = m_clock.reserveEpoch();
    m_clock.armSwitch();

    StreamSwitch::Request request;
    request.kind = StreamSwitch::Kind::Flush;
    request.epoch = epoch;
    request.primaryPipe = primaryPipe;
    request.fadeIn = fadeInDuration;

    // Flush-wins supersede policy: an outstanding request is voided in the
    // same sequence step; a voided crossfade's primary pipe is carried over
    // so the audio thread still installs it.
    if (primaryPipe == nullptr && m_switch.isPending()) {
        const auto outstanding = m_switch.peek();
        if (outstanding.kind == StreamSwitch::Kind::Crossfade) {
            request.primaryPipe = outstanding.primaryPipe;
        }
    }
    std::ignore = m_switch.supersede(request);
    return epoch;
}

bool Mixer::isCrossfadePending() const noexcept
{
    return m_switch.isPending();
}

bool Mixer::isCrossfading() const noexcept
{
    return m_fading.isActive() || (m_switch.isPending() && m_switch.peek().kind == StreamSwitch::Kind::Crossfade);
}

bool Mixer::isFadingPipe(const Pipe<float> *pipe) const noexcept
{
    return pipe != nullptr && (m_fading.hasPipe(pipe) || m_switch.hasFadingPipe(pipe));
}

std::span<const float> Mixer::render(std::size_t maxSamples, std::chrono::microseconds estimatedPts)
{
    const int sampleRate = std::max(1, m_clock.currentSampleRate());
    const int channels = std::max(1, m_clock.currentChannels());

    consumeSwitchIfPending(sampleRate, channels);

    const std::span<float> renderSpan = acquireRenderBuffer(maxSamples, channels);
    if (renderSpan.empty()) {
        return {};
    }

    const std::size_t primaryRead = renderPrimary(renderSpan, channels);
    const bool holdForSlowLoad = (primaryRead == 0) && m_primary.isWaitingForFirstSample();
    const std::size_t afterFading = m_fading.mixInto(renderSpan, primaryRead, renderSpan.size(), holdForSlowLoad, channels);
    const std::size_t totalOutput = m_flushTail.mixInto(renderSpan, std::max(primaryRead, afterFading), channels);

    if (totalOutput == 0) {
        noteUnderrunIfUnexpected();
        return {};
    }

    const auto output = renderSpan.first(totalOutput);
    forwardToFft(output, estimatedPts);
    return output;
}

void Mixer::consumeSwitchIfPending(int sampleRate, int channels)
{
    if (!m_switch.poll()) {
        return;
    }

    // The request stays pending until acknowledge() so the pipeline's
    // crossfade wait only returns once the switch is fully handled.
    struct AcknowledgeGuard {
        StreamSwitch &sw;
        ~AcknowledgeGuard() noexcept
        {
            sw.acknowledge();
        }
    };

    const auto request = m_switch.take();
    AcknowledgeGuard ack{m_switch};

    switch (request.kind) {
    case StreamSwitch::Kind::Flush:
        doFlush(request, sampleRate, channels);
        break;
    case StreamSwitch::Kind::Crossfade:
        latchArmedCrossfade(request, sampleRate);
        break;
    case StreamSwitch::Kind::None:
        break;
    }
}

std::span<float> Mixer::acquireRenderBuffer(std::size_t maxSamples, int channels)
{
    const auto ch = static_cast<std::size_t>(channels);
    std::size_t budget = (maxSamples / ch) * ch;
    if (budget == 0) {
        return {};
    }

    if (m_clock.advanceBoundaries(false, &budget) == StreamClock::BoundaryAdvance::FormatStall || budget == 0) {
        return {};
    }

    if (m_callbackBuffer.size() < budget) {
        m_callbackBuffer.resize(budget);
    }

    return std::span{m_callbackBuffer}.first(budget);
}

std::size_t Mixer::renderPrimary(std::span<float> dst, int channels)
{
    const std::size_t primaryRead = m_primary.pull(dst, dst.size());
    if (primaryRead == 0) {
        return 0;
    }

    m_clock.noteSamplesRead(primaryRead);
    if (m_clock.advanceBoundaries(true, nullptr) != StreamClock::BoundaryAdvance::Crossed) {
        m_clock.noteSamplesRendered(primaryRead);
    }
    m_primary.applyFade(dst, primaryRead, channels);
    return primaryRead;
}

void Mixer::noteUnderrunIfUnexpected()
{
    const bool inInitialFadeLoad = m_primary.isWaitingForFirstSample() && m_clock.streamSamplesRead() == 0;
    if (!m_clock.endOfStreamPending() && !m_fading.isFadeOutActive() && !inInitialFadeLoad) {
        m_clock.noteUnderrun();
    }
}

void Mixer::doFlush(const StreamSwitch::Request &request, int sampleRate, int channels)
{
    m_flushTail.capture(m_primary, sampleRate, channels);
    flushBody(request.primaryPipe, request.epoch, request.fadeIn, sampleRate);
}

void Mixer::flushBody(Pipe<float> *primaryPipe, std::uint64_t epoch, std::chrono::milliseconds fadeInDuration, int sampleRate)
{
    m_primary.drain();
    m_primary.resetFade();

    if (m_fading.pipe() == primaryPipe) {
        m_fading.reset();
    } else {
        m_fading.retire();
    }

    if (primaryPipe != nullptr && primaryPipe != m_primary.pipe()) {
        m_primary.setPipe(primaryPipe);
    }
    if (fadeInDuration > 0ms && m_primary.pipe() != nullptr) {
        m_primary.install(m_primary.pipe(), fadeInDuration, sampleRate);
    }

    // The staged position offset survives a flush (it encodes the seek
    // target); only a crossfade latch resets it (new track from zero).
    m_clock.beginStream(epoch);
}

void Mixer::latchArmedCrossfade(const StreamSwitch::Request &request, int sampleRate)
{
    const float outgoingGain = m_primary.currentGain();
    const auto fadeIn = std::max(0ms, request.fadeIn);

    m_primary.install(request.primaryPipe, fadeIn, sampleRate);
    FadingChannel::FadingSpec fadingSpec;
    fadingSpec.pipe = request.fadingPipe;
    fadingSpec.fadeOut = request.fadeOut;
    fadingSpec.holdMax = kSlowLoadHoldMax;
    fadingSpec.initialGain = outgoingGain;
    fadingSpec.incomingHasFadeIn = fadeIn > 0ms;
    fadingSpec.sampleRate = sampleRate;
    m_fading.install(fadingSpec);

    m_clock.setPositionOffset(std::chrono::milliseconds{0});
    m_clock.beginStream(request.epoch);
}

void Mixer::forwardToFft(std::span<const float> pcm, std::chrono::microseconds estimatedPts)
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

    auto *fp = m_fftPipe.load(std::memory_order_seq_cst);
    if (fp == nullptr || pcm.empty()) {
        return;
    }

    auto chunks = pcm | std::views::chunk(PcmBlock::MAX_SAMPLES);
    fp->producer().writeSomeWith(std::ranges::size(chunks), [&](std::span<PcmBlock> fb1, std::span<PcmBlock> fb2) {
        auto chunkIt = chunks.begin();
        const auto chunkEnd = chunks.end();
        auto fillBlocks = [&](std::span<PcmBlock> dst) {
            for (auto &block : dst) {
                if (chunkIt == chunkEnd) {
                    break;
                }
                const auto chunk = *chunkIt;
                block.count = chunk.size();
                block.pts = estimatedPts;
                std::ranges::copy(chunk, block.samples.begin());
                ++chunkIt;
            }
        };
        fillBlocks(fb1);
        fillBlocks(fb2);
    });
}

void Mixer::preAllocate(std::size_t maxSamples)
{
    constexpr std::size_t kMinAllocation = 16384;
    const std::size_t allocation = std::max(maxSamples, kMinAllocation);
    m_callbackBuffer.resize(allocation);
}

void Mixer::reset()
{
    m_clock.resetWrittenCounter();

    std::ignore = m_switch.clear();
    m_flushTail.clear();
    m_fading.reset();
    m_primary.resetFade();
}

void Mixer::resetChannels()
{
    m_clock.resetWrittenCounter();
    m_flushTail.clear();
    m_fading.reset();
    m_primary.onFormatReopen(m_clock.currentSampleRate());
}
