/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonaudiosink.h"

#include <QVariant>

#include <pulse/def.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;

class DragonPulseAudioSink : public DragonAudioSink
{
    Q_OBJECT

public:
    explicit DragonPulseAudioSink(QObject *parent, const QVariantList &args);
    ~DragonPulseAudioSink() override;

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
    [[nodiscard]] int audioBufferUs() const override;

    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    void setStreamName(const QString &name) override;

    // Callbacks called from the PA mainloop thread
    static void writeCallback(pa_stream *s, size_t nbytes, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void contextStateCallback(pa_context *c, void *userdata);
    static void drainCallback(pa_stream *s, int success, void *userdata);
    static void underflowCallback(pa_stream *s, void *userdata);
    static void subscribeCallback(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata);
    static void sinkInputInfoCallback(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata);

private:
    [[nodiscard]] bool connectToServer();
    void disconnectFromServer();
    void resetStreamLocked();
    void applyVolume(float linearGain);
    void requestSinkInputInfo();
    [[nodiscard]] qint64 queuedDurationUsLocked() const;

    struct PaState;
    std::unique_ptr<PaState> m_pa;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    std::mutex m_callbackDoneMutex;
    std::condition_variable m_callbackDoneCv;
    std::atomic<int> m_activeCallbacks{0};

    std::atomic<float> m_cachedGain{1.0f};
    std::atomic<bool> m_cachedMuted{false};
    int m_lastSampleRate = 0;
    int m_lastChannels = 0;
};
