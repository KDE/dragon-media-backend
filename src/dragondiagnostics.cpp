/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonplayer_p.h"
#include <dragonsdl/dragondiagnostics.h>

#include <SDL3/SDL_audio.h>

#include <LockFreeSpscQueue.h>

DragonDiagnostics::DragonDiagnostics(DragonPlayer &player)
    : m_player(player)
{
}

int DragonDiagnostics::sdlAudioBufferMs() const
{
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }

    DragonAudioOutput::AudioSession *session = priv->audioOutput->m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return -1;
    }

    const int frames = SDL_GetAudioStreamQueued(session->stream);
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

    return static_cast<int>((static_cast<int64_t>(frames) * 1000) / spec.freq);
}

int DragonDiagnostics::sdlAudioBufferUs() const
{
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }

    DragonAudioOutput::AudioSession *session = priv->audioOutput->m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return -1;
    }

    const int frames = SDL_GetAudioStreamQueued(session->stream);
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
    DragonPlayerPrivate *priv = m_player.d.get();
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
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv || !priv->audioQueue) {
        return 0;
    }
    return priv->audioQueue->get_num_items_ready();
}

std::size_t DragonDiagnostics::fftQueueSize() const
{
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv || !priv->fftQueue) {
        return 0;
    }
    return priv->fftQueue->get_num_items_ready();
}

bool DragonDiagnostics::decodeLoopActive() const
{
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline.decodeLoopActive();
}

bool DragonDiagnostics::hasActiveDecoder() const
{
    DragonPlayerPrivate *priv = m_player.d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline.isActive();
}