/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonplayer_p.h"

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<DragonPlayerPrivate>(this))
{
}

DragonPlayer::~DragonPlayer() = default;

bool DragonPlayer::muted() const
{
    return d->muted();
}
float DragonPlayer::volume() const
{
    return d->volume();
}
QUrl DragonPlayer::source() const
{
    return d->currentSource;
}
QUrl DragonPlayer::nextSource() const
{
    return d->nextSource;
}
DragonPlayer::PlaybackState DragonPlayer::playbackState() const
{
    return d->currentPlaybackState;
}
DragonPlayer::MediaStatus DragonPlayer::status() const
{
    return d->currentStatus;
}
DragonPlayer::Error DragonPlayer::error() const
{
    return d->currentError;
}
int64_t DragonPlayer::duration() const
{
    return d->currentDuration;
}
int64_t DragonPlayer::position() const
{
    return d->position();
}
bool DragonPlayer::seekable() const
{
    return d->currentSeekable;
}
bool DragonPlayer::isAudioActive() const
{
    return d->isAudioActive();
}
DragonPlayer::FftMode DragonPlayer::fftMode() const
{
    return d->fftMode();
}

void DragonPlayer::setMuted(bool muted)
{
    d->setMuted(muted);
}
void DragonPlayer::setVolume(float gain)
{
    d->setVolume(gain);
}
void DragonPlayer::setSource(const QUrl &source)
{
    d->setSource(source);
}
void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    d->setNextSource(nextSource);
}

void DragonPlayer::setPosition(int64_t posMs)
{
    d->setPosition(posMs);
}

void DragonPlayer::setFftMode(FftMode mode)
{
    d->setFftMode(mode);
}

void DragonPlayer::play()
{
    d->play();
}
void DragonPlayer::pause()
{
    d->pause();
}
void DragonPlayer::stop()
{
    d->stop();
}
void DragonPlayer::seek(int64_t posMs)
{
    setPosition(posMs);
}

void DragonPlayer::saveUndoPosition(int64_t posMs)
{
    d->undoPosition = posMs;
}

void DragonPlayer::restoreUndoPosition()
{
    if (d->undoPosition > 0) {
        setPosition(d->undoPosition);
    }
}
