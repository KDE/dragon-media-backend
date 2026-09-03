/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "player/dragonpipe.h"
#include "player/dragonplayer_p.h"
#include <DragonMediaBackend/dragonplayer.h>
#include <DragonMediaBackend/dragonspectrumanalyzer.h>

#include "fft/dragonfftpipeline.h"
#include "fft/dragonpcmblock.h"

#include <QMetaObject>
#include <QPointer>

class DragonSpectrumAnalyzerPrivate
{
public:
    explicit DragonSpectrumAnalyzerPrivate(DragonPlayer *player)
        : player(player)
    {
    }

    QPointer<DragonPlayer> player;
    DragonPipe<DragonPcmBlock> fftPipe{256};
    DragonFftPipeline fftPipeline{&fftPipe};
    DragonSpectrumAnalyzer::Mode currentMode = DragonSpectrumAnalyzer::Mode::Off;
    int currentRate = 60;

    void syncFormat()
    {
        auto *priv = player ? player->d.get() : nullptr;
        if (priv) {
            fftPipeline.setSampleRate(priv->currentSampleRate);
            fftPipeline.setChannelCount(priv->currentChannels);
        }
    }

    void attachTap()
    {
        auto *priv = player ? player->d.get() : nullptr;
        if (priv && priv->audioSink()) {
            priv->audioSink()->setFftPipe(&fftPipe);
        }
    }

    void detachTap()
    {
        auto *priv = player ? player->d.get() : nullptr;
        if (priv && priv->audioSink()) {
            priv->audioSink()->setFftPipe(nullptr);
        }
    }
};

DragonSpectrumAnalyzer::DragonSpectrumAnalyzer(DragonPlayer *player, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<DragonSpectrumAnalyzerPrivate>(player))
{
    d->fftPipeline.setFrameCallback([this](DragonFftFrame frame) {
        QMetaObject::invokeMethod(
            this,
            [this, f = std::move(frame)]() mutable {
                Q_EMIT frameReady(f);
            },
            Qt::QueuedConnection);
    });

    connect(player, &DragonPlayer::trackChanged, this, [this]() {
        if (d->currentMode != DragonSpectrumAnalyzer::Mode::Off) {
            d->syncFormat();
            d->fftPipeline.restart();
        }
    });

    connect(player, &DragonPlayer::stateChanged, this, [this](DragonPlayer::PlaybackState newState, DragonPlayer::PlaybackState) {
        if (newState == DragonPlayer::PlaybackState::StoppedState && d->currentMode != DragonSpectrumAnalyzer::Mode::Off) {
            d->fftPipeline.stop();
            Q_EMIT activeChanged(false);
        } else if (newState == DragonPlayer::PlaybackState::PlayingState && d->currentMode != DragonSpectrumAnalyzer::Mode::Off) {
            d->syncFormat();
            d->attachTap();
            d->fftPipeline.restart();
            Q_EMIT activeChanged(true);
        }
    });
}

DragonSpectrumAnalyzer::~DragonSpectrumAnalyzer()
{
    if (d->currentMode != DragonSpectrumAnalyzer::Mode::Off) {
        d->fftPipeline.stop();
        d->detachTap();
    }
}

DragonPlayer *DragonSpectrumAnalyzer::player() const
{
    return d->player.data();
}

DragonSpectrumAnalyzer::Mode DragonSpectrumAnalyzer::mode() const
{
    return d->currentMode;
}

int DragonSpectrumAnalyzer::frameRate() const
{
    return d->currentRate;
}

bool DragonSpectrumAnalyzer::isActive() const
{
    return d->currentMode != DragonSpectrumAnalyzer::Mode::Off && d->player && d->player->playbackState() == DragonPlayer::PlaybackState::PlayingState;
}

void DragonSpectrumAnalyzer::setMode(Mode mode)
{
    if (d->currentMode == mode) {
        return;
    }

    const bool wasOff = d->currentMode == DragonSpectrumAnalyzer::Mode::Off;
    d->currentMode = mode;

    if (mode != DragonSpectrumAnalyzer::Mode::Off) {
        if (wasOff) {
            d->syncFormat();
            d->attachTap();
        }
        d->fftPipeline.setMode(mode);
        if (wasOff && d->player && d->player->playbackState() == DragonPlayer::PlaybackState::PlayingState) {
            d->fftPipeline.restart();
        }
    } else {
        d->fftPipeline.stop();
        d->detachTap();
    }

    Q_EMIT modeChanged(mode);
    Q_EMIT activeChanged(isActive());
}

void DragonSpectrumAnalyzer::setFrameRate(int framesPerSecond)
{
    if (framesPerSecond <= 0) {
        framesPerSecond = 1;
    }
    if (d->currentRate == framesPerSecond) {
        return;
    }
    d->currentRate = framesPerSecond;
    d->fftPipeline.setFftRate(framesPerSecond);

    Q_EMIT frameRateChanged(framesPerSecond);
}

std::size_t DragonSpectrumAnalyzer::fftPipeReady() const
{
    return d->fftPipe.consumer().ready();
}
