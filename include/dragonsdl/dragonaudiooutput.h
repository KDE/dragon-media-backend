/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <QObject>
#include <QString>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>

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

    void setQueue(LockFreeSpscQueue<float> *queue);

    void start(int sampleRate, int channels);

    void stop();

    void reset();

    [[nodiscard]] float volume() const;
    void setVolume(float linearGain);

    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);

    void setStreamName(const QString &name);

    [[nodiscard]] int64_t positionMs() const;

    [[nodiscard]] int64_t totalSamplesWritten() const;

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();

private:
    void pumpLoop(std::stop_token st);

    LockFreeSpscQueue<float> *m_audioQueue = nullptr;
    SDL_AudioStream *m_stream = nullptr;
    SDL_AudioDeviceID m_deviceId = 0;

    int m_channels = 2;
    int m_sampleRate = 44100;

    std::atomic<int64_t> m_totalSamplesWritten{0};

    float m_volume = 1.0f;
    bool m_muted = false;

    std::jthread m_pumpThread;
    std::stop_source m_pumpStopSource;
};