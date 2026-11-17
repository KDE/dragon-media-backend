/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include "sink/audiosink.h"
#include "sink/streamclock.h"
#include "stagedflush.h"
#include "voicechannel.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

template<typename T>
class Pipe;
struct PcmBlock;
class AudioSinkStream;

/**
 * Crossfade coordination between the incoming (primary) and outgoing
 * (fading) voice.
 *
 * Heap-allocated and shared via std::shared_ptr between the incoming primary
 * voice and any fading voice demoted against it. If the fading voice fails
 * callback quiescence and is leaked as a zombie, its shared_ptr keeps the link
 * alive even after the incoming primary voice is later destroyed.
 */
struct CrossfadeLink {
    std::atomic<bool> incomingStarted{false};
    std::atomic<bool> outgoingActive{true};
};

/**
 * One audible playback voice: a stream clock, a fixed-pipe channel, an
 * optional backend stream, and the client-side fade envelopes.
 *
 * A voice starts as *pending*: pipe and clock exist, native format and
 * backend stream do not. PlaybackPipeline::open() sets the format, creates
 * the stream and applies the pending fade-in. The voices are owned by
 * PlaybackPipeline on the Qt main thread; render() (through the
 * AudioSinkStreamRenderer interface) runs on the voice's backend audio
 * callback thread. Mixing and resampling of simultaneous voices is the audio
 * server's job.
 *
 * There is no cancellation tail, supersede policy or acknowledgement
 * protocol: hard switches, seeks, stop and load aborts destroy the voice
 * synchronously on the main thread.
 */
class DRAGONMEDIABACKEND_EXPORT Voice : public AudioSinkStreamRenderer
{
public:
    static constexpr std::chrono::milliseconds kSlowLoadHoldMax{2000};

    // Invoked on the audio callback thread; implementations must be
    // lock-free and post to the owning thread themselves (the pipeline's
    // closures use queued invocations carrying the voice generation).
    struct Notifiers {
        std::function<void(std::uint64_t generation, std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration, std::uint64_t streamEpoch)>
            trackStarted;
        std::function<void(std::uint64_t generation, int sampleRate, int channels, std::uint64_t streamEpoch)> formatChangeRequired;
        std::function<void(std::uint64_t generation)> fadingFinished;
    };

    Voice(Pipe<float> *pipe, std::uint64_t generation, Notifiers notifiers);
    ~Voice() override;

    Voice(const Voice &) = delete;
    Voice &operator=(const Voice &) = delete;
    Voice(Voice &&) = delete;
    Voice &operator=(Voice &&) = delete;

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return m_generation.load(std::memory_order_acquire);
    }
    void setGeneration(std::uint64_t generation) noexcept
    {
        m_generation.store(generation, std::memory_order_release);
    }
    [[nodiscard]] Pipe<float> *pipe() const noexcept
    {
        return m_channel.pipe();
    }
    [[nodiscard]] StreamClock &clock() noexcept
    {
        return m_clock;
    }
    [[nodiscard]] const StreamClock &clock() const noexcept
    {
        return m_clock;
    }

    // --- Pending-voice publication (quiesced, Qt main thread) ---

    // Publish the voice's initial stream: reserve an epoch, arm the switch,
    // stage the position offset, and publish synchronously. Because the
    // object is not yet concurrently accessed this is quiesced
    // initialization, not an audio-thread latch; the producer gate is open
    // afterwards so the decoder may fill the pipe while loading completes.
    [[nodiscard]] std::uint64_t publishInitialStream(std::chrono::milliseconds positionOffset);

    // Remember the fade-in to apply once the native sample rate is known
    // (staged by armCrossfade/hard-switch before the load completes).
    void setPendingFadeIn(std::chrono::milliseconds duration) noexcept;
    [[nodiscard]] std::chrono::milliseconds pendingFadeIn() const noexcept;

    // --- Format and backend stream (Qt main thread) ---

    // Set the native format and prepare for (re)opening the stream: clock
    // format, written-counter rewind, fade-in rescaling. Quiesced (the old
    // stream, if any, must be closed).
    void setFormat(int sampleRate, int channels);
    [[nodiscard]] AudioSinkStream *stream() const noexcept;
    void configureFadeIn(std::chrono::milliseconds fadeIn);
    void setStream(std::unique_ptr<AudioSinkStream> stream);
    void installStream(std::unique_ptr<AudioSinkStream> stream, std::chrono::milliseconds fadeIn);
    /**
     * Closes and drops the stream (quiescing its callback), keeping pipe,
     * clock, and fade-in state for reopen.
     *
     * If the backend callback fails to quiesce, the backend leaks its native
     * stream; this method releases m_stream without destroying it and marks the
     * voice permanently poisoned (sticky) returning false. Callers must leak
     * a poisoned Voice rather than destroying it (~Voice()) or running
     * resetForTeardown()/runFlushQuiesced(), so a stuck callback can never
     * dereference freed stream, channel, clock, or pipe state.
     */
    [[nodiscard]] bool closeStream();
    [[nodiscard]] bool isPoisoned() const noexcept
    {
        return m_poisoned;
    }

    // --- Flush (seek) on the primary voice ---

    [[nodiscard]] std::uint64_t stageFlush(std::chrono::milliseconds fadeIn);
    // Execute the same flush synchronously with the callback quiesced
    // (paused or closed stream). Returns the reserved epoch.
    [[nodiscard]] std::uint64_t runFlushQuiesced(std::chrono::milliseconds fadeIn);
    // True while a staged flush has not been consumed by the render callback.
    [[nodiscard]] bool isFlushPending() const noexcept;

    // Quiesced teardown reset (after closeStream()): discard any staged
    // flush, clear the fade-in, and fully reset the stream clock (which
    // un-sticks a stale Claimed/Emitted EOS while preserving the producer
    // epoch and sample counters for diagnostics). The voice keeps its pipe,
    // format and generation for the next stream.
    void resetForTeardown();

    // --- Crossfade (Qt main thread) ---

    [[nodiscard]] std::shared_ptr<CrossfadeLink> crossfadeLink() const noexcept
    {
        return m_crossfadeLink;
    }
    // Demote this voice to fading: publish the crossfade link and the
    // fade-out request (configured from the current gain, preserving a
    // partially complete fade-in). Called on the outgoing primary voice.
    void demoteToFading(std::shared_ptr<CrossfadeLink> link, std::chrono::milliseconds fadeOut, bool incomingHasFadeIn);

    // --- Audio callback thread (AudioSinkStreamRenderer) ---

    [[nodiscard]] std::span<const float> render(std::size_t maxSamples, std::chrono::microseconds estimatedPts = {}) override;
    [[nodiscard]] bool endOfStreamPending() const override;
    [[nodiscard]] bool endOfStreamArmed() const override;
    [[nodiscard]] bool tryClaimDrained() override;
    [[nodiscard]] bool takeDrainedEmission() override;

    // --- FFT tap (primary voice only) ---

    // Attach/detach the visualization pipe. Detaching waits for in-flight
    // writers so the pipe can be handed to another voice.
    void setFftPipe(Pipe<PcmBlock> *pipe);

    // Pre-size the render buffer so the first audio callback never
    // allocates; maxSamples == 0 skips preallocation.
    void preAllocate(std::size_t maxSamples);

private:
    friend class TestPlaybackPipeline;

    void clearPoisonForTestCleanup() noexcept
    {
        m_poisoned = false;
    }

    enum class Role : std::uint8_t {
        Primary,
        Fading,
    };

    struct FadeOutRequest {
        std::chrono::milliseconds duration{0};
        std::chrono::milliseconds holdMax{0};
        float initialGain = 1.0f;
        bool incomingHasFadeIn = false;
    };

    void consumePendingFlush(bool applyFadeIn = true);
    void consumeFadeOutRequest();
    void consumeFlushBody(std::uint64_t epoch, std::chrono::milliseconds fadeIn, bool applyFadeIn);
    [[nodiscard]] bool crossfadeHoldForSlowLoad() const noexcept;
    void noteUnderrunIfUnexpected();
    void checkFadingFinished();
    void forwardToFft(std::span<const float> pcm, std::chrono::microseconds pts);
    void ensureRenderStorage(std::size_t budget);
    [[nodiscard]] std::span<float> renderSpan(std::size_t budget)
    {
        return std::span{m_renderBuffer}.first(budget);
    }

    // Declaration order matters: the stream is destroyed (and its callback
    // quiesced) before the channel and the clock it renders against.
    StreamClock m_clock;
    VoiceChannel m_channel;
    std::unique_ptr<AudioSinkStream> m_stream;

    std::atomic<std::uint64_t> m_generation;
    Notifiers m_notifiers;

    std::atomic<Role> m_role{Role::Primary};
    std::shared_ptr<CrossfadeLink> m_crossfadeLink;
    std::shared_ptr<CrossfadeLink> m_demotedLinkOwner;
    std::atomic<CrossfadeLink *> m_demotedLink{nullptr};

    // Fade-out command: published once per voice lifetime by the Qt main
    // thread, consumed once by the audio thread. The payload lives in the
    // voice (stable storage); only the pointer is exchanged.
    FadeOutRequest m_fadeOutCommand{};
    std::atomic<const FadeOutRequest *> m_fadeOutRequest{nullptr};

    StagedFlush m_stagedFlush;

    std::chrono::milliseconds m_pendingFadeIn{0};
    bool m_poisoned = false;

    std::atomic<bool> m_fadingFinishedPosted{false};

    std::atomic<Pipe<PcmBlock> *> m_fftPipe{nullptr};
    std::atomic<int> m_fftWriters{0};

    std::vector<float> m_renderBuffer;
};
