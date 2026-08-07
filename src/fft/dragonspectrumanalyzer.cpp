/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "player/dragonplayer_p.h"
#include <DragonMultimedia/dragonplayer.h>
#include <DragonMultimedia/dragonspectrumanalyzer.h>

#include <QMetaObject>

class DragonSpectrumAnalyzerPrivate
{
public:
    explicit DragonSpectrumAnalyzerPrivate(DragonPlayer *player)
        : player(player)
    {
    }

    DragonPlayer *player;
    DragonSpectrumAnalyzer::Mode currentMode = DragonSpectrumAnalyzer::Mode::Off;
    int currentRate = 60;
};

DragonSpectrumAnalyzer::DragonSpectrumAnalyzer(DragonPlayer *player, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<DragonSpectrumAnalyzerPrivate>(player))
{
    auto *priv = player->d.get();
    if (priv) {
        priv->fftPipeline.setFrameCallback([this](DragonFftFrame frame) {
            QMetaObject::invokeMethod(
                this,
                [this, f = std::move(frame)]() mutable {
                    Q_EMIT frameReady(f);
                },
                Qt::QueuedConnection);
        });
    }
}

DragonSpectrumAnalyzer::~DragonSpectrumAnalyzer()
{
    if (d->player) {
        auto *priv = d->player->d.get();
        if (priv) {
            priv->fftPipeline.stop();
            if (priv->audioOutput) {
                priv->audioSink()->setFftPipe(nullptr);
            }
        }
    }
}

DragonPlayer *DragonSpectrumAnalyzer::player() const
{
    return d->player;
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

    d->currentMode = mode;

    auto *priv = d->player ? d->player->d.get() : nullptr;
    if (priv) {
        priv->audioSink()->setFftPipe(mode != Mode::Off ? &priv->fftPipe : nullptr);
        priv->fftPipeline.setMode(mode);
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

    auto *priv = d->player ? d->player->d.get() : nullptr;
    if (priv) {
        priv->fftPipeline.setFftRate(framesPerSecond);
    }

    Q_EMIT frameRateChanged(framesPerSecond);
}
