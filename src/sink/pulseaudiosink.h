/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "callbackquiesce.h"

#include <QVariant>

#include <pulse/def.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <unordered_map>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;
class PulseAudioSink;

/**
 * One pa_stream (sink input) on the sink's shared threaded mainloop/context,
 * rendering one voice at its native format. The PulseAudio server mixes and
 * resamples the concurrent streams.
 */
class PulseAudioSinkStream : public AudioSinkStream
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PulseAudioSinkStream)

public:
    PulseAudioSinkStream(PulseAudioSink &sink, int sampleRate, int channels, AudioSinkStreamRenderer &renderer);
    ~PulseAudioSinkStream() override;

    void pause() override;
    void resume() override;
    [[nodiscard]] bool isPaused() const override;

    void setGain(float linearGain) override;
    void setMuted(bool muted) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

    // PA callbacks, called from the mainloop thread with this stream as
    // userdata.
    static void writeCallback(pa_stream *s, size_t nbytes, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void drainCallback(pa_stream *s, int success, void *userdata);
    static void underflowCallback(pa_stream *s, void *userdata);
    static void sinkInputInfoCallback(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata);

    [[nodiscard]] uint32_t sinkInputIndex() const
    {
        return m_sinkInputIndex.load(std::memory_order_acquire);
    }

private:
    friend class PulseAudioSink;

    [[nodiscard]] bool connectStream();
    [[nodiscard]] bool closeStream() override;

    void applyVolume(float linearGain);
    // Must be called holding the sink's mainloop lock (PA context calls
    // require it, and m_infoOp is guarded by it).
    void requestSinkInputInfo();
    // Under the sink's mainloop lock (used for the shared name fan-out).
    void updateStreamNameLocked(const QString &mediaName);
    [[nodiscard]] std::optional<std::chrono::microseconds> queuedDurationLocked() const;
    void handleSinkInputInfo(const pa_sink_input_info *info);

    PulseAudioSink &m_sink;
    const int m_sampleRate;
    const int m_channels;

    struct PaStream;
    std::unique_ptr<PaStream> m_stream;

    // Published under the mainloop lock in connectStream(); read from
    // PA-mainloop callbacks (which hold the lock) and from main-thread
    // methods under the lock, but atomic so every reader gets a consistent
    // value regardless.
    std::atomic<uint32_t> m_sinkInputIndex{static_cast<uint32_t>(-1)};

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    // Quiesce handshake between the PA-mainloop-thread callbacks and
    // closeStream().
    CallbackQuiesce m_quiesce;

    // Pending PA operations whose dispatch callbacks carry this stream as
    // userdata; guarded by the sink's mainloop lock. They are cancelled (which
    // nulls the operation's callback/userdata) before this object is freed so
    // a late reply cannot dispatch into freed memory.
    pa_operation *m_infoOp = nullptr;
    pa_operation *m_drainOp = nullptr;

    std::atomic<float> m_cachedGain{1.0f};
    std::atomic<bool> m_cachedMuted{false};
    std::atomic<bool> m_gainSetByPipeline{false};
    std::atomic<bool> m_firstVolumeReportSeen{false};
};

/**
 * PulseAudio backend connection: one pa_threaded_mainloop + pa_context with
 * a shared subscription dispatcher for the whole player; one pa_stream per
 * voice. The subscription callback routes sink-input events by index to the
 * owning stream; stream close removes that stream from the server but keeps
 * the connection alive.
 */
class PulseAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PulseAudioSink)

public:
    explicit PulseAudioSink(QObject *parent, const QVariantList &args);
    ~PulseAudioSink() override;

    [[nodiscard]] bool probe() override;
    [[nodiscard]] std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer) override;

    void setStreamName(const QString &name) override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

private:
    friend class PulseAudioSinkStream;

    // PA callbacks with the sink (not a stream) as userdata.
    static void contextStateCallback(pa_context *c, void *userdata);
    static void subscribeCallback(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata);

    struct ConnectError {
        QString message;
        // true: the server refused or dropped the setup round-trip, a retry
        // may succeed. false: local API failure, do not retry.
        bool retryable = false;
    };
    using ConnectResult = std::expected<void, ConnectError>;

    [[nodiscard]] ConnectResult connectToServer();
    void disconnectFromServer();

    void registerStream(PulseAudioSinkStream *stream);
    void unregisterStream(PulseAudioSinkStream *stream);

    struct PaState;
    std::unique_ptr<PaState> m_pa;

    bool m_poisoned = false;
};
