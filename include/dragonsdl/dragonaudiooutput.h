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

template<typename T>
class LockFreeSpscQueue;

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

    void setQueue(LockFreeSpscQueue<std::float32_t> *queue);
    void setFftQueue(LockFreeSpscQueue<std::float32_t> *queue);
    std::condition_variable *fftCv()
    {
        return &m_fftWaitCv;
    }

    void start(int sampleRate, int channels);

    void stop();

    void pause();

    void resume();

    void reset();

    void setPositionOffset(int64_t offsetMs);

    [[nodiscard]] bool isDeviceOpen() const;

    static void SDLCALL audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);
    [[nodiscard]] float volume() const;
    void setVolume(float linearGain);
    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);
    void setStreamName(const QString &name);

    [[nodiscard]] int64_t positionMs() const;

    [[nodiscard]] int64_t totalSamplesWritten() const;

    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();

private:
    struct AudioSession {
        SDL_AudioStream *stream = nullptr;
        int channels = 2;
        int sampleRate = 44100;
        SDL_AudioDeviceID deviceId = 0;
    };

    std::atomic<AudioSession *> m_session{nullptr};

    std::atomic<LockFreeSpscQueue<std::float32_t> *> m_audioQueue{nullptr};
    std::atomic<LockFreeSpscQueue<std::float32_t> *> m_fftQueue{nullptr};
    std::condition_variable m_fftWaitCv;

    std::atomic<int64_t> m_totalSamplesWritten{0};
    std::atomic<int64_t> m_positionOffsetMs{0};

    float m_volume = 1.0f;
    bool m_muted = false;

    std::atomic<int> m_activeCallbacks{0};
};