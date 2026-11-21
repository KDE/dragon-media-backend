/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "callbackquiesce.h"

#include <QVariant>

#include <pipewire/pipewire.h>

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <vector>

class PipeWireAudioSink;

/**
 * One pw_stream on the sink's shared thread loop/context/core, rendering one
 * voice at its native format. The PipeWire server mixes and resamples the
 * concurrent streams.
 */
class PipeWireAudioSinkStream : public AudioSinkStream
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PipeWireAudioSinkStream)

public:
    PipeWireAudioSinkStream(PipeWireAudioSink &sink, int sampleRate, int channels, AudioSinkStreamRenderer &renderer);
    ~PipeWireAudioSinkStream() override;

    void pause() override;
    void resume() override;
    [[nodiscard]] bool isPaused() const override;

    void setGain(float linearGain) override;
    void setMuted(bool muted) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

    // pw events, called from the pw thread-loop callback thread with this
    // stream as userdata.
    static void onProcess(void *userdata);
    static void onStateChanged(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error);
    static void onControlInfo(void *userdata, uint32_t id, const struct pw_stream_control *control);
    static void onParamChanged(void *userdata, uint32_t id, const struct spa_pod *param);
    static void onDrained(void *userdata);

private:
    friend class PipeWireAudioSink;

    [[nodiscard]] bool connectToCore(StreamVolumeInit volumeInit);
    [[nodiscard]] bool closeStream() override;
    void setChannelVolumes(float linearGain);
    void handleSinkInputVolumes(float avgGain);
    void handleSinkInputMuted(bool muted);

    PipeWireAudioSink &m_sink;
    const int m_sampleRate;
    const int m_channels;

    struct PwStream;
    std::unique_ptr<PwStream> m_stream;
    struct spa_hook m_streamListener{};

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    // Quiesce handshake between the onProcess data thread and closeStream().
    CallbackQuiesce m_quiesce;

    std::vector<float> m_volumesScratch;

    std::atomic<float> m_cachedGain{1.0f};
    std::atomic<bool> m_cachedMuted{false};
    std::atomic<bool> m_gainSetByPipeline{false};
    std::atomic<bool> m_firstVolumeReportSeen{false};
};

/**
 * PipeWire backend connection: one pw_thread_loop with a shared
 * pw_context/pw_core for the whole player; one pw_stream per voice.
 * Stream close removes that stream from the server but keeps the connection
 * alive; a later createStream() may re-establish the connection if it was
 * lost.
 */
class PipeWireAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PipeWireAudioSink)

public:
    explicit PipeWireAudioSink(QObject *parent, const QVariantList &args);
    ~PipeWireAudioSink() override;

    [[nodiscard]] bool probe() override;
    [[nodiscard]] std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit = {}) override;

    void setStreamName(const QString &name) override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

private:
    friend class PipeWireAudioSinkStream;

    struct PwState;
    [[nodiscard]] bool ensureConnected();
    void disconnect();

    // Stream registry for name fan-out and teardown; guarded by the pw
    // thread-loop lock. Entries are removed before stream destruction.
    void registerStream(PipeWireAudioSinkStream *stream);
    void unregisterStream(PipeWireAudioSinkStream *stream);

    std::unique_ptr<PwState> m_pw;

    bool m_poisoned = false;
};
