/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"
#include <stdfloat>

#include <QObject>

#include <SDL3/SDL_audio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

template<typename T>
class DragonPipe;

class DRAGONSDL_EXPORT DragonAudioOutput : public QObject
{
    Q_OBJECT

public:
    explicit DragonAudioOutput(QObject *parent = nullptr);
    ~DragonAudioOutput() override;

    DragonAudioOutput(const DragonAudioOutput &) = delete;
    DragonAudioOutput &operator=(const DragonAudioOutput &) = delete;
    DragonAudioOutput(DragonAudioOutput &&) = delete;
    DragonAudioOutput &operator=(DragonAudioOutput &&) = delete;

    void setAudioPipe(DragonPipe<std::float32_t> *pipe);

    void setFftPipe(DragonPipe<std::float32_t> *pipe);

    void start(int sampleRate, int channels, bool startPaused = false);

    void stop();

    void pause();

    void resume();

    void silence();

    void reset();

    [[nodiscard]] bool isQueueReady() const;
    void setQueueReady(bool ready);

    enum class PositionResetMode {
        Seek,
        GaplessTransition,
        NormalTrackChange
    };

    void setPositionOffset(int64_t offsetMs, PositionResetMode mode = PositionResetMode::NormalTrackChange);

    void clearStream();

    [[nodiscard]] bool isDeviceOpen() const;

    [[nodiscard]] bool isPaused() const;

    static void SDLCALL audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);
    [[nodiscard]] float volume() const;

    void setVolume(float volume);

    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);

    void setStreamName(const QString &name);

    void restoreVolume(SDL_AudioStream *stream);

    [[nodiscard]] int64_t positionMs() const;

    [[nodiscard]] int64_t totalSamplesWritten() const;

    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();

    void audioCallbackInvoked();

    friend class DragonDiagnostics;

private:
    struct AudioSession {
        SDL_AudioStream *stream = nullptr;
        int channels = 2;
        int sampleRate = 44100;
        SDL_AudioDeviceID deviceId = 0;
    };

    std::atomic<AudioSession *> m_session{nullptr};

    std::atomic<DragonPipe<std::float32_t> *> m_audioPipe{nullptr};
    std::atomic<DragonPipe<std::float32_t> *> m_fftPipe{nullptr};

    std::atomic<int64_t> m_totalSamplesWritten{0};
    std::atomic<int64_t> m_positionOffsetMs{0};

    std::atomic<bool> m_flushPending{false};

    std::atomic<bool> m_positionResetPending{false};

    std::atomic<bool> m_queueReady{true};

    float m_volume = 1.0f;
    bool m_muted = false;

    std::atomic<int> m_activeCallbacks{0};
    std::condition_variable m_callbackDoneCv;
    std::mutex m_callbackDoneMutex;
};