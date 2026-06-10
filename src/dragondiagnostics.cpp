/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiosink.h"
#include "dragonplayer_p.h"
#include <DragonMultimedia/dragondiagnostics.h>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

#include <atomic>
#include <cstdint>
#include <memory>

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

    std::atomic<int> m_starvationCount{0};
};

DragonDiagnostics::DragonDiagnostics(DragonPlayer *player)
    : QObject(player)
    , d(std::make_unique<DragonDiagnosticsPrivate>(player))
{
}

DragonDiagnostics::~DragonDiagnostics() = default;

int DragonDiagnostics::audioStarvationCount() const
{
    return d->m_starvationCount;
}

int DragonDiagnostics::sdlAudioBufferUs() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->audioBufferUs();
}

int DragonDiagnostics::sdlAudioBufferFrames() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->audioBufferFrames();
}

std::size_t DragonDiagnostics::decodeQueueSize() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return 0;
    }
    return priv->audioPipe.consumer().ready();
}

std::size_t DragonDiagnostics::fftQueueSize() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return 0;
    }
    return priv->fftPipe.consumer().ready();
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
