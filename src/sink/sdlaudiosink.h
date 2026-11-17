/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "callbackquiesce.h"

#include <SDL3/SDL_audio.h>

#include <QVariant>

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>

class SdlAudioSink;

/**
 * One SDL_AudioStream bound to the sink's shared device, rendering one voice
 * at its native format (SDL converts/resamples into the device format and
 * sums all bound streams).
 *
 * A per-stream paused flag suppresses rendering and outputs silence without
 * advancing the voice's envelopes; when every stream is paused the device
 * itself pauses, preserving immediate silence like the single-stream code.
 */
class SdlAudioSinkStream : public AudioSinkStream
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SdlAudioSinkStream)

public:
    SdlAudioSinkStream(SdlAudioSink &sink,
                       SDL_AudioStream *stream,
                       SDL_AudioDeviceID deviceId,
                       int sampleRate,
                       int channels,
                       AudioSinkStreamRenderer &renderer);
    ~SdlAudioSinkStream() override
    {
        std::ignore = close();
    }

    void pause() override;
    void resume() override;
    [[nodiscard]] bool isPaused() const override;

    void setGain(float linearGain) override;
    void setMuted(bool muted) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

private:
    friend class SdlAudioSink;

    [[nodiscard]] bool closeStream() override;
    void applyStreamGainLocked();

    static void SDLCALL getCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

    SdlAudioSink &m_sink;
    SDL_AudioStream *m_stream;
    SDL_AudioDeviceID m_deviceId;
    const int m_sampleRate;
    const int m_channels;

    float m_gain = 1.0f;
    bool m_muted = false;
    std::atomic<bool> m_paused{false};
    CallbackQuiesce m_quiesce;
};

/**
 * SDL3 backend connection: initializes the SDL audio subsystem once and owns
 * one audio device while at least one stream exists. The device opens on the
 * first createStream() and closes when the last stream closes (which also
 * flushes the disk driver's capture file used by the end-to-end tests).
 */
class SdlAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SdlAudioSink)

public:
    explicit SdlAudioSink(QObject *parent, const QVariantList &args);
    ~SdlAudioSink() override;

    [[nodiscard]] bool probe() override;
    [[nodiscard]] std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit = {}) override;

    void setStreamName(const QString &name) override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

private:
    friend class SdlAudioSinkStream;

    [[nodiscard]] bool openDeviceLocked();
    void closeDeviceLocked();
    void streamClosed(SdlAudioSinkStream *stream, bool quiesced, bool wasPaused);
    void streamPauseChanged(bool paused);

    // Serializes the main-thread control methods (stream creation, device
    // open/close) against stream teardown. Never taken on the RT callback
    // path (the quiesce handshake handles that side).
    mutable std::mutex m_mainMutex;

    SDL_AudioDeviceID m_deviceId = 0;
    int m_liveStreams = 0;
    int m_pausedStreams = 0;

    // Set when a stream close could not quiesce its callback: the native
    // stream and device are deliberately leaked instead of risking a
    // use-after-free, and new streams are refused.
    bool m_poisoned = false;
};

static_assert(sizeof(float) == 4, "we assume 32-bit float");
