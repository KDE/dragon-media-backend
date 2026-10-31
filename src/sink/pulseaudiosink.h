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

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;

class PulseAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PulseAudioSink)

public:
    explicit PulseAudioSink(QObject *parent, const QVariantList &args);
    ~PulseAudioSink() override;

    [[nodiscard]] bool probe() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void setMuted(bool muted) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    void setStreamName(const QString &name) override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

    // Callbacks called from the PA mainloop thread
    static void writeCallback(pa_stream *s, size_t nbytes, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void contextStateCallback(pa_context *c, void *userdata);
    static void drainCallback(pa_stream *s, int success, void *userdata);
    static void underflowCallback(pa_stream *s, void *userdata);
    static void subscribeCallback(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata);
    static void sinkInputInfoCallback(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata);

private:
    struct ConnectError {
        QString message;
        // true: the server refused or dropped the setup round-trip, open() may retry.
        // false: local API failure, do not retry.
        bool retryable = false;
    };
    using ConnectResult = std::expected<void, ConnectError>;

    [[nodiscard]] ConnectResult connectToServer();
    void disconnectFromServer();
    void resetStreamLocked();
    void applyVolume(float linearGain);
    void requestSinkInputInfo();
    [[nodiscard]] std::optional<std::chrono::microseconds> queuedDurationLocked() const;

    struct PaState;
    std::unique_ptr<PaState> m_pa;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    // Quiesce handshake between the PA-mainloop-thread callbacks and close().
    CallbackQuiesce m_quiesce;

    // Set when close() could not quiesce its callbacks within the bounded
    // wait: the PA handles are deliberately leaked into a detached "zombie"
    // state instead of risking a use-after-free.
    bool m_poisoned = false;

    std::atomic<float> m_cachedGain{1.0f};
    std::atomic<bool> m_cachedMuted{false};
    std::atomic<bool> m_gainSetByPipeline{false};
    std::atomic<bool> m_firstVolumeReportSeen{false};
};
