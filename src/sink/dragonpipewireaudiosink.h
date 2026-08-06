/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonaudiosink.h"

#include <QVariant>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

struct pw_thread_loop;
struct pw_stream;
struct spa_hook;

class DragonPipeWireAudioSink : public DragonAudioSink
{
    Q_OBJECT

public:
    explicit DragonPipeWireAudioSink(QObject *parent, const QVariantList &args);
    ~DragonPipeWireAudioSink() override;

    [[nodiscard]] bool probe() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] int audioBufferUs() const override;

    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    // Called from the pw_thread_loop callback thread public for the static events struct
    static void onProcess(void *userdata);
    static void onControlInfo(void *userdata, uint32_t id, const struct pw_stream_control *control);
    static void onParamChanged(void *userdata, uint32_t id, const struct spa_pod *param);
    static void onDrained(void *userdata);

private:
    void setChannelVolumes(float linearGain);
    void resetDrainState() override;

    struct PwState;
    std::unique_ptr<PwState> m_pw;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    std::atomic<bool> m_drainInitiated{false};

    std::mutex m_callbackDoneMutex;
    std::condition_variable m_callbackDoneCv;
    std::atomic<int> m_activeCallbacks{0};

    std::vector<float> m_volumesScratch;

    std::atomic<float> m_cachedGain{1.0f};
};
