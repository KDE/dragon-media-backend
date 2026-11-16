/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>

/**
 * The render surface one backend stream pulls PCM from. Implemented by Voice.
 *
 * The implementation object must stay valid from before createStream() until
 * close() has quiesced the stream's audio callback; the owning pipeline
 * guarantees that ordering (the stream is closed before the voice's channel
 * and clock are destroyed).
 */
class DRAGONMEDIABACKEND_EXPORT AudioSinkStreamRenderer
{
public:
    virtual ~AudioSinkStreamRenderer() = default;

    // Render one quantum of interleaved F32 PCM (up to maxSamples samples).
    // The returned span stays valid until the next call and is owned by the
    // renderer. Returning an empty span means "no data" (starvation or EOS
    // protocol) and the backend decides how to fill the quantum.
    [[nodiscard]] virtual std::span<const float> render(std::size_t maxSamples, std::chrono::microseconds estimatedPts = {}) = 0;

    // End-of-stream drain protocol, consulted by the backend stream when no
    // samples are available.
    [[nodiscard]] virtual bool endOfStreamPending() const = 0;
    [[nodiscard]] virtual bool endOfStreamArmed() const = 0;
    [[nodiscard]] virtual bool tryClaimDrained() = 0;
    [[nodiscard]] virtual bool takeDrainedEmission() = 0;
};

/**
 * One audible stream on the audio server/subsystem. Audio callback threads,
 * per-stream volume/mute/pause, stream naming, queued-data diagnostics and
 * the drain protocol live here. Created only through AudioSink::createStream()
 * and closed exactly once before destruction.
 */
class DRAGONMEDIABACKEND_EXPORT AudioSinkStream : public QObject
{
    Q_OBJECT

public:
    ~AudioSinkStream() override;

    AudioSinkStream(const AudioSinkStream &) = delete;
    AudioSinkStream &operator=(const AudioSinkStream &) = delete;
    AudioSinkStream(AudioSinkStream &&) = delete;
    AudioSinkStream &operator=(AudioSinkStream &&) = delete;

    // Quiesce the audio callback and remove the stream from the server.
    // Idempotent. Returns false when the callback did not quiesce within the
    // bounded wait: the backend then leaks its native state into a zombie
    // safeguard, and the caller must keep the renderer (the whole voice)
    // alive as well instead of freeing it under the stuck callback.
    [[nodiscard]] bool close();

    virtual void pause() = 0;
    virtual void resume() = 0;
    [[nodiscard]] virtual bool isPaused() const = 0;

    virtual void setGain(float linearGain) = 0;
    virtual void setMuted(bool muted) = 0;

    virtual void clearStream() = 0;
    [[nodiscard]] virtual qint64 deviceQueuedSamples() const = 0;
    [[nodiscard]] virtual int audioBufferFrames() const = 0;
    [[nodiscard]] virtual std::optional<std::chrono::microseconds> audioBufferDuration() const = 0;

Q_SIGNALS:
    // Stream-level failure (lost server stream, write error). Connection-level
    // failures go through AudioSink::errorOccurred.
    void errorOccurred(const QString &message);

    // Emitted by the backend when this stream has fully played out its
    // end-of-stream (after claiming it from the renderer).
    void drained();

    // External (server-side) volume/mute changes observed by the backend,
    // emitted from the backend's callback thread. Connections are
    // auto-queued to the receiver's thread.
    void externalVolumeChanged(float linearGain);
    void externalVolumeRestored(float linearGain);
    void externalMutedChanged(bool muted);

protected:
    explicit AudioSinkStream(AudioSinkStreamRenderer &renderer, QObject *parent = nullptr);

    [[nodiscard]] AudioSinkStreamRenderer &renderer() const
    {
        return m_renderer;
    }

private:
    [[nodiscard]] virtual bool closeStream() = 0;

    AudioSinkStreamRenderer &m_renderer;
    bool m_closed = false;
    bool m_quiesced = true;
};

/**
 * SPI for one audio output backend (SDL3, PipeWire, PulseAudio, null).
 *
 * A sink owns exactly the connection to the audio server/subsystem: reaching
 * it, keeping it alive, and handing out one AudioSinkStream per audible
 * voice at that voice's native format. The server mixes and resamples the
 * per-voice streams; per-stream sample-domain state (fades, clocks, pipes)
 * lives in Voice, and the glue between the two is PlaybackPipeline.
 *
 * Backends loaded as plugins implement this class (with a
 * QObject*, QVariantList constructor registered via K_PLUGIN_CLASS_WITH_JSON).
 */
class DRAGONMEDIABACKEND_EXPORT AudioSink : public QObject
{
    Q_OBJECT

public:
    explicit AudioSink(QObject *parent = nullptr);
    ~AudioSink() override;

    AudioSink(const AudioSink &) = delete;
    AudioSink &operator=(const AudioSink &) = delete;
    AudioSink(AudioSink &&) = delete;
    AudioSink &operator=(AudioSink &&) = delete;

    // Runtime probe: call after construction to verify the backend can reach
    // its audio subsystem (e.g. PipeWire socket, SDL audio device). Returns
    // false if this sink will never be able to connect. The factory discards
    // plugins that fail probe() and tries the next one.
    [[nodiscard]] virtual bool probe() = 0;

    // Create one server-side stream at the voice's native format. A success
    // result means the server accepted a usable stream, not merely that a
    // client object was allocated; failure leaves no registered stream.
    [[nodiscard]] virtual std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer) = 0;

    // Live-update the user-visible stream name on every active stream (and
    // remember it for streams created later).
    virtual void setStreamName(const QString &name);
    [[nodiscard]] QString streamName() const
    {
        return m_streamName;
    }
    [[nodiscard]] static QString defaultStreamName();
    [[nodiscard]] static QString applicationIconName();
    [[nodiscard]] QString resolvedStreamName() const;

    // Preferred render-buffer size in interleaved samples for the given
    // format; backends pre-size their transfer buffers with it so the first
    // audio callback never allocates. 0 = no preference.
    [[nodiscard]] virtual std::size_t preferredCallbackSamples(int sampleRate, int channels) const;

Q_SIGNALS:
    // Connection-level failures (no per-stream voice to blame). Stream-level
    // failures are reported through the stream's errorOccurred.
    void errorOccurred(const QString &message);

private:
    QString m_streamName;
};
