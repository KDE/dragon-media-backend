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
#include <mutex>

class SdlAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SdlAudioSink)

public:
    explicit SdlAudioSink(QObject *parent, const QVariantList &args);
    ~SdlAudioSink() override;

    [[nodiscard]] bool probe() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void clearStream() override;
    [[nodiscard]] qint64 deviceQueuedSamples() const override;
    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    void setStreamName(const QString &name) override;

    // Diagnostic helpers
    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override;

    [[nodiscard]] std::size_t preferredCallbackSamples(int sampleRate, int channels) const override;

private:
    static void SDLCALL audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

    struct AudioSession {
        SDL_AudioStream *stream = nullptr;
        SDL_AudioDeviceID deviceId = 0;
    };

    std::atomic<AudioSession *> m_session{nullptr};

    // Serializes the main-thread control methods (pause/resume/setGain/
    // clearStream/deviceQueuedSamples) against close()'s teardown of the
    // native session. Never taken on the RT audio callback path.
    mutable std::mutex m_mainMutex;

    // Quiesce handshake between the RT audio callback and close().
    CallbackQuiesce m_quiesce;

    // Set when close() could not quiesce its callback within the bounded
    // wait: the native session is deliberately leaked into a detached
    // "zombie" state instead of risking a use-after-free.
    bool m_poisoned = false;

    std::atomic<float> m_cachedGain{1.0f};
};
