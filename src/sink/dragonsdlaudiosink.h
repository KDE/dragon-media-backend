/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonaudiosink.h"

#include <SDL3/SDL_audio.h>

#include <QVariant>

#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <mutex>

class DragonDiagnostics;

class DragonSdlAudioSink : public DragonAudioSink
{
    Q_OBJECT

public:
    explicit DragonSdlAudioSink(QObject *parent, const QVariantList &args);
    ~DragonSdlAudioSink() override;

    [[nodiscard]] bool probe() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void clearStream() override;
    [[nodiscard]] int64_t deviceQueuedSamples() const override;
    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    void setStreamName(const QString &name) override;

    void notifyDecodeFinished() override;
    void resetDrainState() override;

    // Diagnostic helpers
    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] int audioBufferUs() const override;

private:
    static void SDLCALL audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

    struct AudioSession {
        SDL_AudioStream *stream = nullptr;
        SDL_AudioDeviceID deviceId = 0;
    };

    std::atomic<AudioSession *> m_session{nullptr};

    std::mutex m_callbackDoneMutex;
    std::condition_variable m_callbackDoneCv;
    std::atomic<int> m_activeCallbacks{0};
    float m_cachedGain = 1.0f;

    QTimer *m_drainTimer = nullptr;
    static constexpr int kDrainPollMs = 50;

    friend class DragonDiagnostics;
};
