/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"
#include "sink/dragonaudiosink.h"
#include <DragonMultimedia/dragondiagnostics.h>

#include <memory>

class DragonDiagnosticsPrivate
{
public:
    explicit DragonDiagnosticsPrivate(DragonPlayer *player)
        : m_player(player)
    {
    }

    DragonPlayer *m_player;
};

DragonDiagnostics::DragonDiagnostics(DragonPlayer *player)
    : QObject(player)
    , d(std::make_unique<DragonDiagnosticsPrivate>(player))
{
}

DragonDiagnostics::~DragonDiagnostics() = default;

int DragonDiagnostics::audioBufferUs() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->sink()->audioBufferUs();
}

int DragonDiagnostics::audioBufferFrames() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->sink()->audioBufferFrames();
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

bool DragonDiagnostics::isAudioActive() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    bool hasOutput = (priv->audioOutput && priv->audioOutput->sink());
    bool isOpen = hasOutput ? priv->audioOutput->sink()->isDeviceOpen() : false;
    return hasOutput && isOpen && priv->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState;
}

int DragonDiagnostics::audioUnderrunCount() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return 0;
    }
    return priv->audioOutput->sink()->underrunCount();
}
