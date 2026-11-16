/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "diagnostics.h"
#include "dragonplayer_p.h"
#include "sink/playbackpipeline.h"
#include <dragonspectrumanalyzer.h>

#include <QPointer>
#include <memory>

class DiagnosticsPrivate
{
public:
    explicit DiagnosticsPrivate(DragonPlayer *player)
        : m_player(player)
    {
    }

    DragonPlayer *m_player;
    QPointer<DragonSpectrumAnalyzer> m_analyzer;
};

Diagnostics::Diagnostics(DragonPlayer *player)
    : QObject(player)
    , d(std::make_unique<DiagnosticsPrivate>(player))
{
}

Diagnostics::~Diagnostics() = default;

void Diagnostics::setSpectrumAnalyzer(DragonSpectrumAnalyzer *analyzer)
{
    d->m_analyzer = analyzer;
}

std::optional<std::chrono::microseconds> Diagnostics::audioBufferDuration() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return std::nullopt;
    }
    return priv->audioOutput->playbackPipeline()->audioBufferDuration();
}

int Diagnostics::audioBufferFrames() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->playbackPipeline()->audioBufferFrames();
}

std::size_t Diagnostics::decodeQueueSize() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return 0;
    }
    return priv->audioPipe().consumer().ready();
}

qint64 Diagnostics::audioSamplesRead() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return -1;
    }
    return priv->audioOutput->playbackPipeline()->streamSamplesRead();
}

std::size_t Diagnostics::fftQueueSize() const
{
    if (!d->m_analyzer) {
        return 0;
    }
    return d->m_analyzer->fftPipeReady();
}

bool Diagnostics::decodeLoopActive() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline().decodeLoopActive();
}

bool Diagnostics::hasActiveDecoder() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    return priv->decodePipeline().isActive();
}

bool Diagnostics::isAudioActive() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return false;
    }
    bool hasOutput = (priv->audioOutput && priv->audioOutput->playbackPipeline());
    bool isOpen = hasOutput ? priv->audioOutput->playbackPipeline()->isDeviceOpen() : false;
    // During a crossfade's loading window the pending primary has no stream
    // yet, but the outgoing voice is still audibly fading.
    bool isFading = hasOutput ? priv->audioOutput->playbackPipeline()->isCrossfading() : false;
    return hasOutput && (isOpen || isFading) && priv->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState;
}

int Diagnostics::audioUnderrunCount() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return 0;
    }
    return priv->audioOutput->playbackPipeline()->underrunCount();
}

std::chrono::milliseconds Diagnostics::audioPosition() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput || !priv->audioOutput->playbackPipeline()) {
        return std::chrono::milliseconds{0};
    }
    return priv->audioOutput->playbackPipeline()->position();
}

bool Diagnostics::isQueueReady() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput || !priv->audioOutput->playbackPipeline()) {
        return false;
    }
    return priv->audioOutput->playbackPipeline()->isQueueReady();
}

bool Diagnostics::isFlushPending() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput || !priv->audioOutput->playbackPipeline()) {
        return false;
    }
    return priv->audioOutput->playbackPipeline()->isFlushPending();
}

qint64 Diagnostics::producerSamples() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput || !priv->audioOutput->playbackPipeline()) {
        return -1;
    }
    return priv->audioOutput->playbackPipeline()->producerSamples();
}

std::uint64_t Diagnostics::streamEpoch() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput || !priv->audioOutput->playbackPipeline()) {
        return 0;
    }
    return priv->audioOutput->playbackPipeline()->streamEpoch();
}

std::uint64_t Diagnostics::primarySlotStreamEpoch() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv) {
        return 0;
    }
    return priv->primarySlot().streamEpoch.load(std::memory_order_acquire);
}

PlaybackPipeline *Diagnostics::playbackPipeline() const
{
    DragonPlayerPrivate *priv = d->m_player->d.get();
    if (!priv || !priv->audioOutput) {
        return nullptr;
    }
    return priv->audioOutput->playbackPipeline();
}
