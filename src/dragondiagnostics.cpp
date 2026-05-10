/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonplayer_p.h"
#include <dragonsdl/dragondiagnostics.h>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

#include <LockFreeSpscQueue.h>

#include <atomic>
#include <cstdint>

class DragonDiagnosticsPrivate
{
public:
    explicit DragonDiagnosticsPrivate(DragonPlayer *player)
        : m_player(player)
    {
    }

    DragonPlayer *m_player;

    std::atomic<std::uint64_t> m_callbackCount{0};
    std::atomic<std::uint64_t> m_callbackTimestampUs{0};
    std::atomic<float> m_callbackHz{0.0f};
};

DragonDiagnostics::DragonDiagnostics(DragonPlayer *player)
    : QObject(player)
    , d(std::make_unique<DragonDiagnosticsPrivate>(player))
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (priv && priv->audioOutput) {
        QObject::connect(
            priv->audioOutput.get(),
            &DragonAudioOutput::audioCallbackInvoked,
            this,
            [this]() {
                const uint64_t count = d->m_callbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((count % 50) == 0) {
                    const uint64_t now = SDL_GetTicksNS() / 1000ULL;
                    const uint64_t lastTime = d->m_callbackTimestampUs.load(std::memory_order_relaxed);
                    if (lastTime > 0) {
                        const uint64_t deltaUs = now - lastTime;
                        if (deltaUs > 0) {
                            d->m_callbackHz.store(50000000.0f / static_cast<float>(deltaUs), std::memory_order_relaxed);
                        }
                    }
                    d->m_callbackTimestampUs.store(now, std::memory_order_relaxed);
                }
            },
            Qt::DirectConnection);
    }
}

DragonDiagnostics::~DragonDiagnostics() = default;

int DragonDiagnostics::sdlAudioBufferUs() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }

    DragonAudioOutput::AudioSession *session = priv->audioOutput->m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return -1;
    }

    const int frames = sdlAudioBufferFrames();
    if (frames <= 0) {
        return 0;
    }

    SDL_AudioDeviceID deviceId = SDL_GetAudioStreamDevice(session->stream);
    SDL_AudioSpec spec;
    if (!SDL_GetAudioDeviceFormat(deviceId, &spec, nullptr)) {
        return -1;
    }

    if (spec.freq <= 0) {
        return -1;
    }

    return static_cast<int>((static_cast<int64_t>(frames) * 1000000) / spec.freq);
}

int DragonDiagnostics::sdlAudioBufferFrames() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }

    DragonAudioOutput::AudioSession *session = priv->audioOutput->m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return -1;
    }

    const int frames = SDL_GetAudioStreamQueued(session->stream);
    return frames > 0 ? frames : 0;
}

std::size_t DragonDiagnostics::decodeQueueSize() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioQueue) {
        return 0;
    }
    return priv->audioQueue->get_num_items_ready();
}

std::size_t DragonDiagnostics::fftQueueSize() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->fftQueue) {
        return 0;
    }
    return priv->fftQueue->get_num_items_ready();
}

bool DragonDiagnostics::decodeLoopActive() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline.decodeLoopActive();
}

bool DragonDiagnostics::hasActiveDecoder() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline.isActive();
}

float DragonDiagnostics::audioCallbackHz() const
{
    return d->m_callbackHz.load(std::memory_order_relaxed);
}
