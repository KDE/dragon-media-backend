/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonaudiosink.h"

#include <QVariant>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;
struct pa_sample_spec;
struct pa_channel_map;

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
    void clearStream() override;
    [[nodiscard]] int64_t deviceQueuedSamples() const override;

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

private:
    [[nodiscard]] bool connectToServer();
    void disconnectFromServer();
    void applyVolume(float linearGain);
    void resetDrainState() override;

    std::atomic<bool> m_drainRequested{false};

    struct PaState;
    std::unique_ptr<PaState> m_pa;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    std::mutex m_callbackDoneMutex;
    std::condition_variable m_callbackDoneCv;
    std::atomic<int> m_activeCallbacks{0};

    std::atomic<float> m_cachedGain{1.0f};
    std::string m_streamName;
    int m_lastSampleRate = 0;
    int m_lastChannels = 0;
};
