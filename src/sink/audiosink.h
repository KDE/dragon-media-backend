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
#include <optional>

class Mixer;

/**
 * SPI for one audio output backend (SDL3, PipeWire, PulseAudio, null).
 *
 * A sink owns exactly the backend-specific concerns: reaching the audio
 * server/subsystem, running the audio callback thread, server-side volume/
 * mute/stream naming, and its buffer diagnostics. All stream mixing, stream
 * clock tracking, crossfading, and position accounting live in Mixer;
 * the glue between the two is PlaybackPipeline.
 *
 * Backends loaded as plugins implement this class (with a
 * QObject*, QVariantList constructor registered via K_PLUGIN_CLASS_WITH_JSON)
 * and render from the mixer set by setMixer() inside their audio callbacks.
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

    virtual void open(int sampleRate, int channels) = 0;
    virtual void close() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void setGain(float linearGain) = 0;
    virtual void clearStream() = 0;
    [[nodiscard]] virtual qint64 deviceQueuedSamples() const = 0;
    [[nodiscard]] virtual bool isDeviceOpen() const = 0;
    [[nodiscard]] virtual bool isPaused() const = 0;

    // Server-side mute for backends that have one (PipeWire, PulseAudio).
    // The SDL and null backends handle muting purely through setGain().
    virtual void setMuted(bool muted);

    [[nodiscard]] virtual std::optional<std::chrono::microseconds> audioBufferDuration() const
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual int audioBufferFrames() const
    {
        return -1;
    }

    virtual void setStreamName(const QString &name);
    [[nodiscard]] QString streamName() const
    {
        return m_streamName;
    }
    [[nodiscard]] static QString defaultStreamName();
    [[nodiscard]] static QString applicationIconName();
    [[nodiscard]] QString resolvedStreamName() const;

    // Preferred render-buffer size in interleaved samples for the given
    // format; PlaybackPipeline preallocates the mixer's render buffers with it
    // so the first audio callback never allocates. 0 = no preallocation.
    [[nodiscard]] virtual std::size_t preferredCallbackSamples(int sampleRate, int channels) const;

    // The mixing engine this sink's audio callback renders from. Set by the
    // owning PlaybackPipeline before the sink is used, and valid for the
    // sink's whole lifetime within that pipeline (the pipeline destroys the sink
    // before the mixer).
    void setMixer(Mixer *mixer);
    [[nodiscard]] Mixer *mixer() const;

Q_SIGNALS:
    void errorOccurred(const QString &message);

    // Emitted by the backend when EOS has fully played out (after claiming it
    // from the mixer's EOS state).
    void drained();

    // External (server-side) volume/mute changes observed by the backend,
    // emitted from the backend's callback thread. Connections are
    // auto-queued to the receiver's thread.
    void externalVolumeChanged(float linearGain);
    void externalVolumeRestored(float linearGain);
    void externalMutedChanged(bool muted);

private:
    Mixer *m_mixer = nullptr;

    QString m_streamName;
};
