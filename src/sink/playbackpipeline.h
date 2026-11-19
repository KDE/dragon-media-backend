/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "dragonmediabackend_export.h"
#include "streamclock.h"
#include "voice/voice.h"

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

template<typename T>
class Pipe;
struct PcmBlock;

/**
 * Glue between the audio-output parts: the backend connection (one AudioSink),
 * the primary Voice and the optional fading Voice (each a StreamClock, a
 * fixed pipe and its own server-side AudioSinkStream), and the volume/mute
 * policy (slider <-> linear gain curve).
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

    // The owned backend connection (SDL3 / PipeWire / PulseAudio / null).
    [[nodiscard]] AudioSink *sink() const
    {
        return m_sink.get();
    }

    // The current primary voice's backend stream (null while the primary is
    // pending or its stream is closed). Narrow test/diagnostics surface:
    // production code talks to the voices through the pipeline.
    [[nodiscard]] AudioSinkStream *primaryStream() const
    {
        return m_primary ? m_primary->stream() : nullptr;
    }

    [[nodiscard]] bool probe() const
    {
        return m_sink->probe();
    }

    // Create or reopen only the primary stream (a pending primary gets its
    // first stream; a running primary is reopened at the new format). The
    // fading voice and its stream are untouched.
    void open(int sampleRate, int channels);
    [[nodiscard]] bool close()
    {
        return !m_primary || m_primary->closeStream();
    }
    // Both voice streams pause; callbacks stop pulling samples so the
    // crossfade freezes.
    void pause();
    void resume();
    void clearStream();
    [[nodiscard]] qint64 deviceQueuedSamples() const;
    [[nodiscard]] bool isDeviceOpen() const
    {
        return m_primary && m_primary->stream() != nullptr;
    }
    [[nodiscard]] bool isPaused() const;

    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const;
    [[nodiscard]] int audioBufferFrames() const;
    [[nodiscard]] int underrunCount() const
    {
        return m_primary ? m_primary->clock().underrunCount() : 0;
    }

    // Quiesced-only contract: install the pipe a fresh pending primary voice
    // renders from, destroying any fading voice. Called before any stream
    // exists or while the audio callback is stopped.
    void setAudioPipe(Pipe<float> *pipe);
    // Attach the single visualization pipe to the primary voice (nullptr
    // detaches, waiting for in-flight writers).
    void setFftPipe(Pipe<PcmBlock> *pipe);

    void closeGate()
    {
        if (m_primary) {
            m_primary->clock().closeGate();
        }
    }
    bool reopenIfIdle()
    {
        return m_primary && m_primary->clock().reopenIfIdle();
    }
    [[nodiscard]] bool isQueueReady() const
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return !m_primary || m_primary->clock().isQueueReady();
    }
    [[nodiscard]] bool isQueueReadyForPipe(const Pipe<float> *pipe) const
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return !m_primary || m_primary->pipe() != pipe || m_primary->clock().isQueueReady();
    }
    // True while the primary voice has an armed (staged, unconsumed) flush.
    [[nodiscard]] bool isFlushPending() const
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->clock().isSwitchArmed();
    }

    // Publish the given position offset and apply it either as a hard switch
    // (different non-null pipe: the pending replacement voice's initial
    // epoch is returned), a deferred flush consumed by the audio thread
    // (running stream), or a synchronous flush (paused/closed stream).
    [[nodiscard]] std::uint64_t setPositionOffset(std::chrono::milliseconds offset,
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
        return m_primary && m_primary->clock().hasFormat(sampleRate, channels);
    }
    [[nodiscard]] qint64 totalSamplesWritten() const
    {
        return m_primary ? m_primary->clock().totalSamplesWritten() : 0;
    }
    void resetPositionTracking()
    {
        if (m_primary) {
            m_primary->clock().resetPositionTracking();
        }
    }

    [[nodiscard]] qint64 streamSamplesRead() const
    {
        return m_primary ? m_primary->clock().streamSamplesRead() : 0;
    }

    [[nodiscard]] qint64 producerSamples() const
    {
        return m_primary ? m_primary->clock().producerSamples() : 0;
    }

    [[nodiscard]] std::chrono::milliseconds position() const;

    // Destroy the fading voice and reset the primary voice in place (its
    // callback quiesced): only ever called with the audio callbacks stopped
    // (after close()). The clock reset un-sticks a stale Claimed/Emitted EOS
    // so the next stream can arm end-of-stream again, while the producer
    // epoch and sample counters survive for diagnostics.
    void reset();

    [[nodiscard]] bool
    boundaryTrackStart(std::uint32_t trackId, int sampleRate, int channels, std::optional<std::chrono::milliseconds> duration, std::uint64_t epochTag)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->clock().boundaryTrackStart(trackId, sampleRate, channels, duration, epochTag);
    }

    [[nodiscard]] bool boundaryTrackStartForPipe(const Pipe<float> *pipe,
                                                 std::uint32_t trackId,
                                                 int sampleRate,
                                                 int channels,
                                                 std::optional<std::chrono::milliseconds> duration,
                                                 std::uint64_t epochTag)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->pipe() == pipe && m_primary->clock().boundaryTrackStart(trackId, sampleRate, channels, duration, epochTag);
    }

    [[nodiscard]] bool boundaryEndOfStream(std::uint64_t epochTag)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->clock().boundaryEndOfStream(epochTag);
    }

    [[nodiscard]] bool boundaryEndOfStreamForPipe(const Pipe<float> *pipe, std::uint64_t epochTag)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->pipe() == pipe && m_primary->clock().boundaryEndOfStream(epochTag);
    }

    [[nodiscard]] bool retractEndOfStream()
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->clock().retractEndOfStream();
    }

    [[nodiscard]] bool retractEndOfStreamForPipe(const Pipe<float> *pipe)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary && m_primary->pipe() == pipe && m_primary->clock().retractEndOfStream();
    }

    void samplesProduced(std::uint64_t epochTag, std::size_t count)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        if (m_primary) {
            m_primary->clock().samplesProduced(epochTag, count);
        }
    }

    void samplesProducedForPipe(const Pipe<float> *pipe, std::uint64_t epochTag, std::size_t count)
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        if (m_primary && m_primary->pipe() == pipe) {
            m_primary->clock().samplesProduced(epochTag, count);
        }
    }

    [[nodiscard]] std::uint64_t streamEpoch() const
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return m_primary ? m_primary->clock().streamEpoch() : 0;
    }

    [[nodiscard]] std::uint64_t streamEpochForPipe(const Pipe<float> *pipe) const
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        return (m_primary && m_primary->pipe() == pipe) ? m_primary->clock().streamEpoch() : 0;
    }

    [[nodiscard]] bool endOfStreamPending() const
    {
        return m_primary && m_primary->clock().endOfStreamPending();
    }

    [[nodiscard]] int currentSampleRate() const
    {
        return m_primary ? m_primary->clock().currentSampleRate() : 0;
    }
    [[nodiscard]] int currentChannels() const
    {
        return m_primary ? m_primary->clock().currentChannels() : 0;
    }

    [[nodiscard]] bool endOfStreamArmed() const
    {
        return m_primary && m_primary->clock().endOfStreamArmed();
    }

    [[nodiscard]] bool tryClaimDrained()
    {
        return m_primary && m_primary->clock().tryClaimDrained();
    }

    [[nodiscard]] bool takeDrainedEmission()
    {
        return m_primary && m_primary->clock().takeDrainedEmission();
    }

    // Arm a crossfade and return the (voice-local) epoch the incoming stream
    // publishes under, minted synchronously at staging time. Creates the
    // pending incoming Voice — no backend stream exists for it until open()
    // learns its native format. The outgoing voice keeps its stream and gets
    // a fade-out request configured from its current gain.
    [[nodiscard]] std::uint64_t
    armCrossfade(Pipe<float> *fadingPipe, Pipe<float> *newPrimaryPipe, std::chrono::milliseconds fadeOutDuration, std::chrono::milliseconds fadeInDuration);

    // A fading Voice exists.
    [[nodiscard]] bool isCrossfading() const noexcept
    {
        return m_fading != nullptr;
    }

    [[nodiscard]] bool isFadingPipe(const Pipe<float> *pipe) const noexcept
    {
        return pipe != nullptr && m_fading && m_fading->pipe() == pipe;
    }

    [[nodiscard]] bool isPrimaryPipe(const Pipe<float> *pipe) const noexcept
    {
        return pipe != nullptr && m_primary && m_primary->pipe() == pipe;
    }

    // Destroy the fading voice immediately, without the completion callback:
    // for explicit stop, hard switch, seek and teardown, where the player is
    // already performing the matching decoder cancellation.
    void abortFadingVoice()
    {
        destroyFadingVoice();
    }

    // Installed by the player; invoked on the Qt main thread after the
    // fading voice ended (fade completed, pipe drained, or fading-stream
    // error) and was destroyed. The player stops and drains the matching
    // fading slot by pipe identity.
    using FadingVoiceEndedCallback = std::function<void(Pipe<float> *pipe, const QString &error)>;
    void setFadingVoiceEndedCallback(FadingVoiceEndedCallback callback)
    {
        m_fadingVoiceEndedCallback = std::move(callback);
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
    [[nodiscard]] std::unique_ptr<Voice> makeVoice(Pipe<float> *pipe);
    void connectStreamSignals(AudioSinkStream *stream, std::uint64_t generation, Pipe<float> *pipe, bool firstStream);
    void applyGainToVoices();
    void destroyVoices();
    void destroyFadingVoice();
    void dropVoice(std::unique_ptr<Voice> voice);
    std::uint64_t replacePrimaryVoice(Pipe<float> *pipe, std::chrono::milliseconds offset, std::chrono::milliseconds fadeInDuration);
    void onFadingVoiceEnded(Pipe<float> *pipe, const QString &error);

    // Declaration order matters: the fading voice is destroyed before the
    // primary voice (which owns the CrossfadeLink the fading voice's render
    // reads) and before the sink connection.
    std::unique_ptr<AudioSink> m_sink;
    mutable std::mutex m_primaryVoiceMutex;
    std::unique_ptr<Voice> m_primary;
    std::unique_ptr<Voice> m_fading;

    std::uint64_t m_nextVoiceGeneration = 1;
    std::uint64_t m_primaryGeneration = 0;
    std::uint64_t m_fadingGeneration = 0;

    FadingVoiceEndedCallback m_fadingVoiceEndedCallback;

    float m_volume = 1.0f;
    bool m_muted = false;
    // Mirrors the old sink-level gain cache: streams are created with the
    // cached gain only once the user actually changed volume, so a
    // server-restored volume can still be adopted for the first stream.
    bool m_gainSetByPipeline = false;
    bool m_firstStream = true;

    Pipe<PcmBlock> *m_fftPipe = nullptr;
};
