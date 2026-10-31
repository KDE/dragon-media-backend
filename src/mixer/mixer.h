/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "flushtail.h"
#include "dragonmediabackend_export.h"
#include "fadingchannel.h"
#include "primarychannel.h"
#include "sink/streamclock.h"
#include "streamswitch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

template<typename T>
class Pipe;
struct PcmBlock;

/**
 * Audio-thread sample-domain engine: turns pipes into a mixed PCM quantum.
 *
 * Coordinates primary playback (PrimaryChannel), crossfading/superseding
 * (FadingChannel), and cross-thread staging (StreamSwitch).
 * Stream-progress questions (epoch, boundaries, EOS, counters, position,
 * format, underruns) are delegated to StreamClock; render() consults
 * the clock for the read budget, boundary crossings and flushes, and reports
 * what it consumed back to it.
 *
 * Deliberately not a QObject (see StreamClock/PlaybackPipeline for the
 * Qt boundary). Threading contract:
 *  - armCrossfade()/stageFlush()/flushStreams() stage via the
 *    seqlock-protected StreamSwitch mailbox from the Qt main thread,
 *  - render() and the atomic latch it performs run exclusively on the audio
 *    callback thread of the owning sink.
 */
class DRAGONMEDIABACKEND_EXPORT Mixer
{
public:
    explicit Mixer(StreamClock &clock);
    ~Mixer();

    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;
    Mixer(Mixer &&) = delete;
    Mixer &operator=(Mixer &&) = delete;

    // The stream clock this mixer renders against (owned by the playback pipeline).
    [[nodiscard]] StreamClock &clock() const
    {
        return m_clock;
    }

    void setAudioPipe(Pipe<float> *pipe);
    void setFftPipe(Pipe<PcmBlock> *pipe);
    // Install once, before any audio thread runs. Invoked on the audio
    // callback thread after a staged request is consumed.
    void setSwitchAckedCallback(std::function<void()> callback)
    {
        m_switch.setAckedCallback(std::move(callback));
    }

    // Synchronous flush: gate the producers, drain all pipes, tear down any
    // active fade, clear any staged request, and publish a fresh stream on
    // the clock (which reopens the gate). Runs with the audio callback
    // quiesced (the paused/closed seek or setSource path).
    [[nodiscard]] std::uint64_t flushStreams(Pipe<float> *primaryPipe = nullptr, std::chrono::milliseconds fadeInDuration = std::chrono::milliseconds{0});

    static constexpr std::chrono::milliseconds kSlowLoadHoldMax{2000};

    // Arm a crossfade (or fade-in-only when fadingPipe == nullptr): reserves
    // the stream epoch, gates the producers, and stages the request. The
    // epoch is minted here, before anything is published to the audio
    // thread, so the caller can record it before the latch runs.
    // Returns the minted epoch, or std::nullopt when a request is still
    // un-consumed (the caller waits it out or supersedes it).
    [[nodiscard]] std::optional<std::uint64_t>
    armCrossfade(Pipe<float> *fadingPipe, Pipe<float> *newPrimaryPipe, std::chrono::milliseconds fadeOutDuration, std::chrono::milliseconds fadeInDuration);

    // Stage a flush request (seek / hard switch / crossfade abort while the
    // audio thread keeps running). Supersedes any outstanding request in a
    // single sequence step; when the voided request carried a primary pipe
    // (an un-latched crossfade) and primaryPipe is nullptr, that pipe is
    // carried into the flush so the audio thread still installs it. Returns
    // the minted epoch.
    [[nodiscard]] std::uint64_t stageFlush(Pipe<float> *primaryPipe, std::chrono::milliseconds fadeInDuration);

    [[nodiscard]] bool isCrossfadePending() const noexcept;

    [[nodiscard]] bool isCrossfading() const noexcept;

    [[nodiscard]] bool isFadingPipe(const Pipe<float> *pipe) const noexcept;

    // True when the audio thread's primary channel is currently reading this
    // pipe. Main-thread drains must skip primary and fading pipes alike: the
    // SPSC queue allows exactly one consumer.
    [[nodiscard]] bool isPrimaryPipe(const Pipe<float> *pipe) const noexcept
    {
        return pipe != nullptr && m_primary.pipe() == pipe;
    }

    // Clear all switch staging so a stale latch can never install a
    // detached pipe once the device is reopened. Only ever called with the
    // audio callback quiesced (after close()).
    void reset();

    // Quiesced channel-only reset used by PlaybackPipeline::open()
    // (mid-stream format reopen): tears down the fade state and the written
    // counter but leaves any staged switch request intact so it is still
    // consumed (and published) after the device reopens.
    void resetChannels();

    // Pre-size the render buffers for the device's quantum so the first
    // callback never allocates; maxSamples == 0 skips preallocation.
    void preAllocate(std::size_t maxSamples);

    // Mix one quantum of audio from the primary/fading pipes. Audio callback
    // thread only.
    [[nodiscard]] std::span<const float> render(std::size_t maxSamples, std::chrono::microseconds estimatedPts);

private:
    void consumeSwitchIfPending(int sampleRate, int channels);
    [[nodiscard]] std::span<float> acquireRenderBuffer(std::size_t maxSamples, int channels);
    [[nodiscard]] std::size_t renderPrimary(std::span<float> dst, int channels);
    void noteUnderrunIfUnexpected();

    void latchArmedCrossfade(const StreamSwitch::Request &request, int sampleRate);
    void doFlush(const StreamSwitch::Request &request, int sampleRate, int channels);
    void flushBody(Pipe<float> *primaryPipe, std::uint64_t epoch, std::chrono::milliseconds fadeInDuration, int sampleRate);
    void forwardToFft(std::span<const float> pcm, std::chrono::microseconds pts);

    StreamClock &m_clock;

    PrimaryChannel m_primary;
    FadingChannel m_fading;
    FlushTail m_flushTail;
    StreamSwitch m_switch;

    std::atomic<Pipe<PcmBlock> *> m_fftPipe{nullptr};
    std::atomic<int> m_fftWriters{0};
    std::vector<float> m_callbackBuffer;
};
