/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "callbackquiesce.h"

#include <QVariant>

#include <atomic>
#include <cstddef>
#include <vector>

#include <pipewire/pipewire.h>

class PipeWireAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PipeWireAudioSink)

public:
    explicit PipeWireAudioSink(QObject *parent, const QVariantList &args);
    ~PipeWireAudioSink() override;

    [[nodiscard]] bool probe() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void setMuted(bool muted) override;
    void setStreamName(const QString &name) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

    // Called from the pw_thread_loop callback thread public for the static events struct
    static void onProcess(void *userdata);
    static void onStateChanged(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error);
    static void onControlInfo(void *userdata, uint32_t id, const struct pw_stream_control *control);
    static void onParamChanged(void *userdata, uint32_t id, const struct spa_pod *param);
    static void onDrained(void *userdata);

private:
    void setChannelVolumes(float linearGain);

    struct PwState;
    std::unique_ptr<PwState> m_pw;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    // Quiesce handshake between the onProcess data thread and close().
    CallbackQuiesce m_quiesce;

    // Set when close() could not quiesce onProcess within the bounded wait:
    // the pw stream/loop are deliberately leaked into a detached "zombie"
    // state instead of risking a use-after-free.
    bool m_poisoned = false;

    std::vector<float> m_volumesScratch;

    std::atomic<float> m_cachedGain{1.0f};
    std::atomic<bool> m_cachedMuted{false};
    std::atomic<bool> m_gainSetByPipeline{false};
    std::atomic<bool> m_firstVolumeReportSeen{false};
};
