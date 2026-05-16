/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonplayer_p.h"

#include <dragonsdl/dragonplayer.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#include <QCoroTask>
#pragma GCC diagnostic pop
#include <QMetaObject>
#include <QTimer>
#include <dragonsdl_logging.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdfloat>
#include <stop_token>
#include <thread>
#include <utility>

DragonPlayerPrivate::DragonPlayerPrivate(DragonPlayer *player)
    : QObject(player)
    , q(player)
    , decodePipeline(player)
    , aliveGuard(std::make_shared<AliveGuard>())
{
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent)
{
    switch (intent) {
    case DragonPlayer::PlaybackState::PlayingState:
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->start(sampleRate, channels);
        } else {
            audioOutput->resume();
        }
        audioOutput->setQueueReady(true);
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        break;

    case DragonPlayer::PlaybackState::PausedState:
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->start(sampleRate, channels, true);
        } else {
            audioOutput->pause();
        }
        audioOutput->setQueueReady(true);
        setPlaybackState(DragonPlayer::PlaybackState::PausedState);
        break;

    case DragonPlayer::PlaybackState::StoppedState:
        break;
    }
}

void DragonPlayerPrivate::onGaplessTransition(const QUrl &newSource, int sampleRate, int channels, qint64 durationMs)
{
    qCDebug(dragonsdlPlayer) << "onGaplessTransition newSource=" << newSource.toString() << "sr=" << sampleRate << "ch=" << channels
                             << "duration=" << durationMs;

    if (newSource != nextSource) {
        qCDebug(dragonsdlPlayer) << "ignoring stale onGaplessTransition (source mismatch)";
        return;
    }

    if (nextSource.isEmpty()) {
        qCDebug(dragonsdlPlayer) << "onGaplessTransition nextSource was cleared, aborting";
        return;
    }

    currentSource = nextSource;
    nextSource.clear();
    currentPosition = 0;
    currentIsLocal = currentSource.isLocalFile();
    currentSeekable = currentIsLocal;
    currentDuration = durationMs;
    currentSampleRate = sampleRate;
    currentChannels = channels;

    decodePipeline.setCurrentSource(currentSource);

    audioOutput->setPositionOffset(0, DragonAudioOutput::PositionResetMode::GaplessTransition);
    Q_EMIT q->positionChanged(0);

    Q_EMIT q->trackChanged();
    Q_EMIT q->sourceChanged();
    Q_EMIT q->nextSourceChanged();
    Q_EMIT q->seekableChanged(currentSeekable);

    if (currentDuration >= 0) {
        Q_EMIT q->durationChanged(currentDuration);
    }
}

void DragonPlayerPrivate::onDecodeFinished(const QUrl &source, bool hadFatalError)
{
    qCDebug(dragonsdlPlayer) << "onDecodeFinished called source=" << source.toString() << "currentSource=" << currentSource.toString()
                             << "hadFatalError=" << hadFatalError;

    if (source != currentSource) {
        qCDebug(dragonsdlPlayer) << "ignoring stale onDecodeFinished (source mismatch)";
        return;
    }

    if (audioOutput) {
        audioOutput->stop();
        audioOutput->setQueueReady(false);
    }

    if (!hadFatalError && !nextSource.isEmpty()) {
        q->setSource(nextSource);
        return;
    }

    qCDebug(dragonsdlPlayer) << "onDecodeFinished emitting state/status changes, hadFatalError=" << hadFatalError;
    if (hadFatalError) {
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        setStatus(DragonPlayer::MediaStatus::InvalidMedia);
    } else {
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        setStatus(DragonPlayer::MediaStatus::EndOfMedia);
    }
    qCDebug(dragonsdlPlayer) << "onDecodeFinished state/status changes complete";
}

void DragonPlayerPrivate::onDecodeError(const QString &)
{
    auto err = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
    if (currentError != err) {
        currentError = err;
        Q_EMIT q->errorChanged(currentError);
    }
}

void DragonPlayerPrivate::writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st)
{
    if (pcm.empty()) {
        return;
    }

    while (audioOutput && !audioOutput->isQueueReady() && !st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (st.stop_requested()) {
        return;
    }

    audioPipe.producer().write(pcm, st);
}

void DragonPlayerPrivate::setPlaybackState(DragonPlayer::PlaybackState state)
{
    qCDebug(dragonsdlPlayer) << "setPlaybackState(" << state << ") current=" << currentPlaybackState;
    if (currentPlaybackState == state) {
        qCDebug(dragonsdlPlayer) << "setPlaybackState no change, returning";
        return;
    }

    const bool wasPlaying = (currentPlaybackState == DragonPlayer::PlaybackState::PlayingState);
    const bool willBePlaying = (state == DragonPlayer::PlaybackState::PlayingState);

    if (state == DragonPlayer::PlaybackState::PlayingState) {
        if (positionTimer) {
            positionTimer->start();
        }
    } else {
        if (positionTimer) {
            positionTimer->stop();
        }
    }

    currentPlaybackState = state;

    if (wasPlaying || willBePlaying) {
        Q_EMIT q->playingChanged(willBePlaying);
    }

    Q_EMIT q->playbackStateChanged(state);

    switch (state) {
    case DragonPlayer::PlaybackState::PlayingState:
        Q_EMIT q->playing();
        break;
    case DragonPlayer::PlaybackState::PausedState:
        Q_EMIT q->paused();
        break;
    case DragonPlayer::PlaybackState::StoppedState:
        Q_EMIT q->stopped();
        break;
    default:
        std::unreachable();
    }
}

void DragonPlayerPrivate::setStatus(DragonPlayer::MediaStatus status)
{
    qCDebug(dragonsdlPlayer) << "setStatus(" << status << ") current=" << currentStatus;
    if (currentStatus == status) {
        qCDebug(dragonsdlPlayer) << "setStatus no change, returning";
        return;
    }
    currentStatus = status;
    qCDebug(dragonsdlPlayer) << "setStatus emitting statusChanged";
    Q_EMIT q->statusChanged(status);

    if (status == DragonPlayer::MediaStatus::InvalidMedia && currentError != DragonPlayer::Error::FormatError) {
        currentError = DragonPlayer::Error::FormatError;
        Q_EMIT q->errorChanged(currentError);
    }
}

void DragonPlayerPrivate::setError(DragonPlayer::Error error)
{
    if (currentError == error) {
        return;
    }
    currentError = error;
    Q_EMIT q->errorChanged(error);

    if (error != DragonPlayer::Error::NoError && currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
        currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
    }
}

void DragonPlayerPrivate::stopPipeline()
{
    qCDebug(dragonsdlPlayer) << "stopPipeline() full teardown";

    decodePipeline.stop();

    fftPipeline.stop();

    if (audioOutput) {
        audioOutput->stop();
        audioOutput->reset();
    }

    qCDebug(dragonsdlPlayer) << "stopPipeline() teardown complete";
}

void DragonPlayerPrivate::init()
{
    audioOutput = std::make_unique<DragonAudioOutput>();
    audioOutput->setAudioPipe(&audioPipe);
    audioOutput->setFftPipe(nullptr);

    connect(audioOutput.get(), &DragonAudioOutput::errorOccurred, this, [this](const QString &) {
        setError(DragonPlayer::Error::ResourceError);
    });

    connect(audioOutput.get(), &DragonAudioOutput::volumeChanged, q, &DragonPlayer::volumeChanged);

    positionTimer = new QTimer(this);
    positionTimer->setInterval(100);
    connect(positionTimer, &QTimer::timeout, this, [this]() {
        Q_EMIT q->positionChanged(audioOutput && audioOutput->isDeviceOpen() ? audioOutput->positionMs() : currentPosition);
    });

    decodePipeline.setSamplesCallback([this](auto samples, const std::stop_token &st) {
        writeToQueues(samples, st);
    });

    connect(&decodePipeline, &DragonDecodePipeline::sessionError, this, &DragonPlayerPrivate::onDecodeError, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::sessionFinished, this, &DragonPlayerPrivate::onDecodeFinished, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::gaplessTransition, this, &DragonPlayerPrivate::onGaplessTransition, Qt::QueuedConnection);

    connect(
        &decodePipeline,
        &DragonDecodePipeline::bufferProgressChanged,
        this,
        [this](double progress) {
            if (!qFuzzyCompare(currentBufferProgress, progress)) {
                currentBufferProgress = progress;
                Q_EMIT q->bufferProgressChanged(progress);
            }
        },
        Qt::QueuedConnection);

    fftPipeline.setFrameCallback([this](DragonFftFrame frame) {
        QMetaObject::invokeMethod(
            q,
            [this, f = std::move(frame)]() mutable {
                Q_EMIT q->fftFrameReady(f);
            },
            Qt::QueuedConnection);
    });
}

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
{
    d = std::make_unique<DragonPlayerPrivate>(this);
    d->init();
}

DragonPlayer::~DragonPlayer()
{
    if (d && d->aliveGuard) {
        d->aliveGuard->alive = false;
    }
    d->stopPipeline();
}

bool DragonPlayer::muted() const
{
    return d->currentMuted;
}
float DragonPlayer::volume() const
{
    return d->currentVolume;
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
    if (d->audioOutput && d->audioOutput->isDeviceOpen()) {
        return d->audioOutput->positionMs();
    }
    return d->currentPosition;
}
bool DragonPlayer::seekable() const
{
    return d->currentSeekable;
}
bool DragonPlayer::isAudioActive() const
{
    return d->audioOutput && d->audioOutput->isDeviceOpen() && d->currentPlaybackState != PlaybackState::StoppedState;
}
DragonPlayer::FftMode DragonPlayer::fftMode() const
{
    return d->currentFftMode;
}
double DragonPlayer::bufferProgress() const
{
    return d->currentBufferProgress;
}

void DragonPlayer::setMuted(bool muted)
{
    qCDebug(dragonsdlPlayer) << "setMuted(" << muted << ")";
    if (d->currentMuted == muted) {
        return;
    }
    d->currentMuted = muted;
    if (d->audioOutput) {
        d->audioOutput->setMuted(muted);
    }
    Q_EMIT d->q->mutedChanged(muted);
}

void DragonPlayer::setVolume(float gain)
{
    qCDebug(dragonsdlPlayer) << "setVolume(" << gain << ")";
    d->currentVolume = gain;
    if (d->audioOutput) {
        d->audioOutput->setVolume(gain);
    }
}

QCoro::Task<void> DragonPlayer::setSource(QUrl source)
{
    qCDebug(dragonsdlPlayer) << "setSource(" << source.toString() << ")";

    auto aliveGuard = d->aliveGuard;

    if (d->audioOutput) {
        d->audioOutput->silence();
        d->audioOutput->setQueueReady(false);
        d->audioOutput->setPositionOffset(0, DragonAudioOutput::PositionResetMode::NormalTrackChange);
    }

    d->decodePipeline.stopSession();

    if (d->currentFftMode == FftMode::Off) {
        d->fftPipeline.stop();
        d->fftPipeline.teardown();
        if (d->audioOutput) {
            d->audioOutput->setFftPipe(nullptr);
        }
    } else {
        d->fftPipeline.ensureInfrastructure(&d->fftPipe, d->currentFftMode);
        if (d->audioOutput) {
            d->audioOutput->setFftPipe(&d->fftPipe);
        }
        d->fftPipeline.restartThread();
    }

    d->currentSource = source;
    d->currentPosition = 0;
    d->currentDuration = 0;
    d->nextSource.clear();
    d->currentSampleRate = 0;
    d->currentChannels = 0;

    Q_EMIT sourceChanged();
    Q_EMIT nextSourceChanged();

    if (source.isEmpty()) {
        if (d->audioOutput) {
            d->audioOutput->stop();
            d->audioOutput->reset();
        }
        d->setStatus(MediaStatus::NoMedia);
        d->setPlaybackState(PlaybackState::StoppedState);
        co_return;
    }

    if (d->currentPlaybackState != PlaybackState::StoppedState) {
        d->setPlaybackState(PlaybackState::StoppedState);
    } else {
        Q_EMIT playbackStateChanged(PlaybackState::StoppedState);
        Q_EMIT stopped();
    }

    if (d->currentError != Error::NoError) {
        d->currentError = Error::NoError;
        Q_EMIT errorChanged(Error::NoError);
    }

    d->setStatus(MediaStatus::LoadingMedia);

    const bool isLocal = source.isLocalFile();

    d->currentIsLocal = isLocal;
    d->currentSeekable = isLocal;
    Q_EMIT seekableChanged(d->currentSeekable);

    auto result = co_await d->decodePipeline.initializeSession(source);

    if (!aliveGuard || !aliveGuard->alive) {
        co_return;
    }

    if (d->currentSource != source) {
        qCDebug(dragonsdlPlayer) << "superseded setSource coroutine (source" << source.toString() << "!= current" << d->currentSource.toString()
                                 << "), discarding";
        co_return;
    }

    if (!result.success) {
        if (result.cancelled) {
            qCDebug(dragonsdlPlayer) << "setSource coroutine cancelled, returning without state change";
            co_return;
        }
        d->currentError = d->currentIsLocal ? Error::FormatError : Error::NetworkError;
        Q_EMIT errorChanged(d->currentError);
        d->setStatus(MediaStatus::InvalidMedia);
        d->setPlaybackState(PlaybackState::StoppedState);
        co_return;
    }

    d->currentSampleRate = result.sampleRate;
    d->currentChannels = result.channels;
    d->currentDuration = result.durationMs;

    if (d->currentDuration >= 0) {
        Q_EMIT durationChanged(d->currentDuration);
    }

    d->fftPipeline.setSampleRate(result.sampleRate);
    d->fftPipeline.setChannelCount(result.channels);
    d->setStatus(MediaStatus::LoadedMedia);

    d->applyRequestedState(result.sampleRate, result.channels, d->requestedPlaybackState);

    co_return;
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    qCDebug(dragonsdlPlayer) << "setNextSource(" << nextSource.toString() << ")";
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline.setNextSource(nextSource);
}

void DragonPlayer::setPosition(int64_t posMs)
{
    qCDebug(dragonsdlPlayer) << "setPosition(" << posMs << ")";
    posMs = std::clamp(posMs, int64_t{0}, std::max(d->currentDuration, int64_t{0}));
    d->currentPosition = posMs;

    d->decodePipeline.requestSeek(posMs);

    if (d->audioOutput) {
        d->audioOutput->setPositionOffset(posMs, DragonAudioOutput::PositionResetMode::Seek);
        d->audioOutput->clearStream();
    }
    Q_EMIT positionChanged(posMs);

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        d->currentStatus = MediaStatus::LoadedMedia;
        Q_EMIT statusChanged(MediaStatus::LoadedMedia);
    }
}

void DragonPlayer::setFftMode(FftMode mode)
{
    qCDebug(dragonsdlPlayer) << "setFftMode(" << mode << ")";
    if (d->currentFftMode == mode) {
        return;
    }

    const bool wasOn = (d->currentFftMode != DragonPlayer::FftMode::Off);
    const bool nowOn = (mode != DragonPlayer::FftMode::Off);
    d->currentFftMode = mode;

    if (!wasOn && nowOn) {
        d->fftPipeline.ensureInfrastructure(&d->fftPipe, mode);
        if (d->audioOutput) {
            d->audioOutput->setFftPipe(&d->fftPipe);
        }
        if (d->fftPipeline.isRunning()) {
            d->fftPipeline.restartThread();
        } else {
            d->fftPipeline.start();
        }
    } else if (wasOn && !nowOn) {
        if (d->audioOutput) {
            d->audioOutput->setFftPipe(nullptr);
        }
        d->fftPipeline.setMode(mode);
    } else if (nowOn && d->fftPipeline.hasInfrastructure()) {
        d->fftPipeline.setMode(mode);
    }

    Q_EMIT fftModeChanged(mode);
}

void DragonPlayer::play()
{
    qCDebug(dragonsdlPlayer) << "play()";
    if (d->currentSource.isEmpty()) {
        return;
    }

    d->requestedPlaybackState = PlaybackState::PlayingState;

    if (d->currentPlaybackState == PlaybackState::PlayingState) {
        return;
    }

    if (d->currentPlaybackState == PlaybackState::PausedState) {
        if (d->audioOutput) {
            d->audioOutput->resume();
        }
        d->setPlaybackState(PlaybackState::PlayingState);
        return;
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonsdlPlayer) << "play() status is LoadingMedia, intent captured";
        return;
    }

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        qCDebug(dragonsdlPlayer) << "play() status is EndOfMedia, reloading source";
        setSource(d->currentSource);
        return;
    }

    if (!d->decodePipeline.isActive() && !d->currentSource.isEmpty()) {
        qCDebug(dragonsdlPlayer) << "play() decoder not active, restarting decode pipeline";
        setSource(d->currentSource);
        return;
    }

    qCDebug(dragonsdlPlayer) << "play() status is " << d->currentStatus << ", starting audio synchronously";

    if (d->audioOutput && !d->audioOutput->isDeviceOpen() && d->currentSampleRate > 0) {
        d->audioOutput->start(d->currentSampleRate, d->currentChannels);
    }
    if (d->audioOutput) {
        d->audioOutput->setQueueReady(true);
    }
    d->setPlaybackState(PlaybackState::PlayingState);
}

void DragonPlayer::pause()
{
    qCDebug(dragonsdlPlayer) << "pause()";

    if (d->currentStatus == MediaStatus::NoMedia || d->currentStatus == MediaStatus::InvalidMedia) {
        return;
    }

    d->requestedPlaybackState = PlaybackState::PausedState;

    if (d->currentPlaybackState == PlaybackState::PausedState) {
        return;
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonsdlPlayer) << "pause() status is LoadingMedia, intent captured";
        return;
    }

    if (d->audioOutput) {
        d->audioOutput->pause();
    }
    d->setPlaybackState(PlaybackState::PausedState);
}

void DragonPlayer::stop()
{
    qCDebug(dragonsdlPlayer) << "stop()";

    d->requestedPlaybackState = PlaybackState::StoppedState;

    d->decodePipeline.stopSession();

    if (d->audioOutput) {
        d->audioOutput->stop();
        d->audioOutput->reset();
    }
    d->fftPipeline.stop();

    d->setPlaybackState(PlaybackState::StoppedState);

    if (d->currentStatus != MediaStatus::NoMedia && d->currentStatus != MediaStatus::InvalidMedia) {
        if (d->currentStatus != MediaStatus::LoadedMedia) {
            d->currentStatus = MediaStatus::LoadedMedia;
        }
        Q_EMIT statusChanged(MediaStatus::LoadedMedia);
    }
}

void DragonPlayer::seek(int64_t posMs)
{
    qCDebug(dragonsdlPlayer) << "seek(" << posMs << ")";
    setPosition(posMs);
}

void DragonPlayer::saveUndoPosition(int64_t posMs)
{
    qCDebug(dragonsdlPlayer) << "saveUndoPosition(" << posMs << ")";
    d->undoPosition = posMs;
}

void DragonPlayer::restoreUndoPosition()
{
    qCDebug(dragonsdlPlayer) << "restoreUndoPosition()";
    if (d->undoPosition > 0) {
        setPosition(d->undoPosition);
    }
}
