/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "dragonmediabackend_export.h"
#include "mixer/mixer.h"
#include "streamclock.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>

template<typename T>
class Pipe;
struct PcmBlock;
class AudioSink;

/**
 * Glue between the three parts of the audio output: a StreamClock
 * (stream-progress state machine: epoch, boundaries, EOS, position), a
 * Mixer (sample-domain mixing engine), and a AudioSink (one
 * concrete audio backend, selected by the factory).
 */
class DRAGONMEDIABACKEND_EXPORT PlaybackPipeline : public QObject
{
    Q_OBJECT

public:
    explicit PlaybackPipeline(std::unique_ptr<AudioSink> sink, QObject *parent = nullptr);
    ~PlaybackPipeline() override;

    PlaybackPipeline(const PlaybackPipeline &) = delete;
    PlaybackPipeline &operator=(const PlaybackPipeline &) = delete;
    PlaybackPipeline(PlaybackPipeline &&) = delete;
    PlaybackPipeline &operator=(PlaybackPipeline &&) = delete;

    // The owned backend (SDL3 / PipeWire / PulseAudio / null).
    [[nodiscard]] AudioSink *sink() const
    {
        return m_sink.get();
    }

    [[nodiscard]] bool probe() const
    {
        return m_sink->probe();
    }
    void open(int sampleRate, int channels);
    void close()
    {
        m_sink->close();
    }
    void pause()
    {
        m_sink->pause();
    }
    void resume()
    {
        m_sink->resume();
    }
    void clearStream()
    {
        m_sink->clearStream();
    }
    [[nodiscard]] qint64 deviceQueuedSamples() const
    {
        return m_sink->deviceQueuedSamples();
    }
    [[nodiscard]] bool isDeviceOpen() const
    {
        return m_sink->isDeviceOpen();
    }
    [[nodiscard]] bool isPaused() const
    {
        return m_sink->isPaused();
    }

    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const
    {
        return m_sink->audioBufferDuration();
    }
    [[nodiscard]] int audioBufferFrames() const
    {
        return m_sink->audioBufferFrames();
    }
    [[nodiscard]] int underrunCount() const
    {
        return m_clock.underrunCount();
    }

    // Quiesced-only contract: the primary pipe is installed either at
    // construction (before any device exists) or while the audio callback is
    // stopped (the device is closed, e.g. after a teardown flush). While a
    // device is open, the primary pipe only ever changes via the audio
    // thread's switch consumption (latch/flush).
    void setAudioPipe(Pipe<float> *pipe)
    {
        Q_ASSERT(!m_sink || !m_sink->isDeviceOpen());
        m_mixer.setAudioPipe(pipe);
    }
    void setFftPipe(Pipe<PcmBlock> *pipe)
    {
        m_mixer.setFftPipe(pipe);
    }

    void closeGate()
    {
        m_clock.closeGate();
    }
    bool reopenIfIdle()
    {
        return m_clock.reopenIfIdle();
    }
    [[nodiscard]] bool isQueueReady() const
    {
        return m_clock.isQueueReady();
    }
    // True while a switch request (flush/crossfade) is staged and not yet
    // consumed by the audio thread. Diagnostics-facing name kept for the
    // "a deferred flush is pending" check.
    [[nodiscard]] bool isFlushPending() const
    {
        return m_clock.isSwitchArmed() || m_mixer.isCrossfadePending();
    }

    // Publish the given position offset and stage the stream switch that
    // applies it. When the device is open and running, the switch is consumed
    // by the audio thread (deferred flush); when the device is closed or the
    // sink paused, the flush runs synchronously with the callback quiesced and
    // the stream is published before returning. Either way the returned epoch
    // is the one the next stream publishes.
    std::uint64_t setPositionOffset(std::chrono::milliseconds offset,
                                    Pipe<float> *primaryPipe = nullptr,
                                    std::chrono::milliseconds fadeInDuration = std::chrono::milliseconds{0});

    [[nodiscard]] float volume() const;
    void setVolume(float volume);
    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);

    void setStreamName(const QString &name)
    {
        m_sink->setStreamName(name);
    }
    [[nodiscard]] QString streamName() const
    {
        return m_sink->streamName();
    }
    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const
    {
        return m_clock.hasFormat(sampleRate, channels);
    }
    [[nodiscard]] qint64 totalSamplesWritten() const
    {
        return m_clock.totalSamplesWritten();
    }
    void resetPositionTracking()
    {
        m_clock.resetPositionTracking();
    }

    [[nodiscard]] qint64 streamSamplesRead() const
    {
        return m_clock.streamSamplesRead();
    }

    [[nodiscard]] qint64 producerSamples() const
    {
        return m_clock.producerSamples();
    }

    [[nodiscard]] std::chrono::milliseconds position() const;

    // Clear crossfade staging, channel state and the full stream-clock state;
    // only ever called with the audio callback quiesced (after close()).
    // The full clock reset un-sticks a stale Claimed/Emitted EOS so the next
    // stream can arm end-of-stream again.
    void reset()
    {
        m_mixer.reset();
        m_clock.reset();
    }

    [[nodiscard]] bool
    boundaryTrackStart(std::uint32_t trackId, int sampleRate, int channels, std::optional<std::chrono::milliseconds> duration, std::uint64_t epochTag)
    {
        return m_clock.boundaryTrackStart(trackId, sampleRate, channels, duration, epochTag);
    }

    [[nodiscard]] bool boundaryEndOfStream(std::uint64_t epochTag)
    {
        return m_clock.boundaryEndOfStream(epochTag);
    }

    [[nodiscard]] bool retractEndOfStream()
    {
        return m_clock.retractEndOfStream();
    }

    void samplesProduced(std::uint64_t epochTag, std::size_t count)
    {
        m_clock.samplesProduced(epochTag, count);
    }

    [[nodiscard]] std::uint64_t streamEpoch() const
    {
        return m_clock.streamEpoch();
    }

    [[nodiscard]] bool endOfStreamPending() const
    {
        return m_clock.endOfStreamPending();
    }

    [[nodiscard]] int currentSampleRate() const
    {
        return m_clock.currentSampleRate();
    }
    [[nodiscard]] int currentChannels() const
    {
        return m_clock.currentChannels();
    }

    [[nodiscard]] bool endOfStreamArmed() const
    {
        return m_clock.endOfStreamArmed();
    }

    [[nodiscard]] bool tryClaimDrained()
    {
        return m_clock.tryClaimDrained();
    }

    [[nodiscard]] bool takeDrainedEmission()
    {
        return m_clock.takeDrainedEmission();
    }

    [[nodiscard]] bool waitForPendingCrossfade(std::chrono::milliseconds timeout = std::chrono::milliseconds(150));

    // Arm a crossfade and return the epoch the new stream will publish under
    // (minted at staging time, before anything reaches the audio thread). On
    // timeout waiting for a previous switch, the stuck request is superseded
    // with a flush carrying the new primary pipe, so the audio thread installs
    // the rotated slot's pipe either way (one switch path for both outcomes).
    [[nodiscard]] std::uint64_t
    armCrossfade(Pipe<float> *fadingPipe, Pipe<float> *newPrimaryPipe, std::chrono::milliseconds fadeOutDuration, std::chrono::milliseconds fadeInDuration);

    [[nodiscard]] bool isCrossfadePending() const noexcept
    {
        return m_mixer.isCrossfadePending();
    }

    [[nodiscard]] bool isCrossfading() const noexcept
    {
        return m_mixer.isCrossfading();
    }

    [[nodiscard]] bool isFadingPipe(const Pipe<float> *pipe) const noexcept
    {
        return m_mixer.isFadingPipe(pipe);
    }

    [[nodiscard]] bool isPrimaryPipe(const Pipe<float> *pipe) const noexcept
    {
        return m_mixer.isPrimaryPipe(pipe);
    }

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();
    void mutedChanged(bool muted);
    void drained();

    void trackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration);

    void formatChangeRequired(int sampleRate, int channels);

private Q_SLOTS:
    void onExternalVolumeChanged(float linearGain);
    void onExternalVolumeRestored(float linearGain);

private:
    // Declaration order matters: the sink is destroyed before the mixer and
    // the clock, so the sink's audio callbacks (which render from the
    // mixer, which consults the clock) are quiesced by its destructor while
    // both are still alive.
    StreamClock m_clock;
    Mixer m_mixer;
    std::unique_ptr<AudioSink> m_sink;

    float m_volume = 1.0f;
    bool m_muted = false;

    // Crossfade-wait support: gated counting semaphore signaled lock-freely
    // from the audio thread only when waitForPendingCrossfade() is waiting.
    std::atomic<bool> m_switchAckWaiting{false};
    std::counting_semaphore<> m_switchAckSemaphore{0};
};
