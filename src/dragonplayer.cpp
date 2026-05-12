/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonplayer_p.h"

#include <dragonsdl/dragonplayer.h>

#include <QMetaObject>
#include <QTimer>
#include <dragonsdl_logging.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <ranges>
#include <stdfloat>
#include <stop_token>
#include <thread>
#include <utility>

static inline void setCurrentThreadName(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

DragonPlayerPrivate::DragonPlayerPrivate(DragonPlayer *player)
    : QObject(player)
    , q(player)
    , decodePipeline(player)
{
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels)
{
    switch (requestedPlaybackState) {
    case DragonPlayer::PlaybackState::PlayingState:
        requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->start(sampleRate, channels);
        } else {
            audioOutput->resume();
        }
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        break;

    case DragonPlayer::PlaybackState::PausedState:
        requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->start(sampleRate, channels, true);
        } else {
            audioOutput->pause();
        }
        setPlaybackState(DragonPlayer::PlaybackState::PausedState);
        break;

    case DragonPlayer::PlaybackState::StoppedState:
        break;
    }
}

void DragonPlayerPrivate::onFormatReady(int sampleRate, int channels, bool isGapless)
{
    qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels << " isGapless=" << isGapless;
    currentSampleRate = sampleRate;
    currentChannels = channels;

    fftPipeline.setSampleRate(sampleRate);
    fftPipeline.setChannelCount(channels);

    if (currentStatus != DragonPlayer::MediaStatus::LoadedMedia) {
        currentStatus = DragonPlayer::MediaStatus::LoadedMedia;
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::LoadedMedia);
    }

    if (isGapless) {
        if (currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
            if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
                audioOutput->start(sampleRate, channels);
            }
            return;
        }

        if (currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
            if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
                audioOutput->start(sampleRate, channels, true);
            }
            return;
        }

        return;
    }

    applyRequestedState(sampleRate, channels);
}

void DragonPlayerPrivate::onGaplessTransition(const QUrl &newSource)
{
    if (decodePipeline.generation() != currentDecoderGeneration) {
        qCDebug(dragonsdlPlayer) << "ignoring stale gapless transition (generation mismatch)";
        return;
    }

    currentSource = newSource;
    nextSource.clear();
    currentPosition = 0;
    currentIsLocal = newSource.isLocalFile();
    currentSeekable = currentIsLocal;
    currentDuration = 0;

    audioOutput->setPositionOffset(0, DragonAudioOutput::PositionResetMode::GaplessTransition);

    Q_EMIT q->trackChanged();
    Q_EMIT q->sourceChanged();
    Q_EMIT q->nextSourceChanged();
    Q_EMIT q->seekableChanged(currentSeekable);
}

void DragonPlayerPrivate::onDecodeFinished(bool hadFatalError)
{
    if (decodePipeline.isActive()) {
        qCDebug(dragonsdlPlayer) << "ignoring stale onDecodeFinished (new decode session already active)";
        return;
    }

    if (hadFatalError) {
        if (currentError != (currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError)) {
            currentError = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
            Q_EMIT q->errorChanged(currentError);
        }
        if (currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
            currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
            Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
        }
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
    } else {
        if (audioOutput && audioOutput->isDeviceOpen()) {
            audioOutput->pause();
        }

        fftPipeline.stop();

        if (currentStatus != DragonPlayer::MediaStatus::EndOfMedia) {
            currentStatus = DragonPlayer::MediaStatus::EndOfMedia;
            Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::EndOfMedia);
        }
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
    }
}

void DragonPlayerPrivate::onDurationChanged(int64_t dur)
{
    if (currentDuration != dur) {
        currentDuration = dur;
        Q_EMIT q->durationChanged(dur);
    }
}

void DragonPlayerPrivate::onErrorOccurred(const QString &)
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

void DragonPlayerPrivate::connectPipelineSignals()
{
    decodePipeline.setSamplesCallback([this](auto samples, const std::stop_token &st) {
        writeToQueues(samples, st);
    });

    connect(&decodePipeline, &DragonDecodePipeline::formatReady, this, &DragonPlayerPrivate::onFormatReady, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::durationChanged, this, &DragonPlayerPrivate::onDurationChanged, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::errorOccurred, this, &DragonPlayerPrivate::onErrorOccurred, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::finished, this, &DragonPlayerPrivate::onDecodeFinished, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::gaplessTransition, this, &DragonPlayerPrivate::onGaplessTransition, Qt::QueuedConnection);
}

void DragonPlayerPrivate::wireFftCallbacks()
{
    fftPipeline.setFrameCallback([this](DragonFftFrame frame) {
        QMetaObject::invokeMethod(
            q,
            [this, f = std::move(frame)]() mutable {
                Q_EMIT q->fftFrameReady(f);
            },
            Qt::QueuedConnection);
    });
}

void DragonPlayerPrivate::setPlaybackState(DragonPlayer::PlaybackState state)
{
    if (currentPlaybackState == state) {
        return;
    }

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
    if (currentStatus == status) {
        return;
    }
    currentStatus = status;
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

    QObject::connect(audioOutput.get(), &DragonAudioOutput::errorOccurred, this, [this](const QString &) {
        setError(DragonPlayer::Error::ResourceError);
    });

    QObject::connect(audioOutput.get(), &DragonAudioOutput::volumeChanged, q, &DragonPlayer::volumeChanged);

    positionTimer = new QTimer(this);
    positionTimer->setInterval(100);
    QObject::connect(positionTimer, &QTimer::timeout, this, [this]() {
        Q_EMIT q->positionChanged(audioOutput && audioOutput->isDeviceOpen() ? audioOutput->positionMs() : currentPosition);
    });

    connectPipelineSignals();
    wireFftCallbacks();
}

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
{
    d = std::make_unique<DragonPlayerPrivate>(this);
    d->init();
}

DragonPlayer::~DragonPlayer()
{
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
    return d->audioOutput && d->audioOutput->isDeviceOpen() && d->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState;
}
DragonPlayer::FftMode DragonPlayer::fftMode() const
{
    return d->currentFftMode;
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

void DragonPlayer::setSource(const QUrl &source)
{
    qCDebug(dragonsdlPlayer) << "setSource(" << source.toString() << ")";

    if (d->audioOutput) {
        d->audioOutput->silence();
        d->audioOutput->setQueueReady(false);
        d->audioOutput->setPositionOffset(0, DragonAudioOutput::PositionResetMode::NormalTrackChange);
    }

    d->decodePipeline.stopSession();

    if (d->currentFftMode == DragonPlayer::FftMode::Off) {
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
        d->setStatus(DragonPlayer::MediaStatus::NoMedia);
        d->setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        return;
    }

    if (d->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState) {
        d->setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
    } else {
        Q_EMIT playbackStateChanged(DragonPlayer::PlaybackState::StoppedState);
        Q_EMIT stopped();
    }

    if (d->currentError != DragonPlayer::Error::NoError) {
        d->currentError = DragonPlayer::Error::NoError;
        Q_EMIT errorChanged(DragonPlayer::Error::NoError);
    }

    d->setStatus(DragonPlayer::MediaStatus::LoadingMedia);

    const bool isLocal = source.isLocalFile();
    d->currentIsLocal = isLocal;
    d->currentSeekable = isLocal;
    Q_EMIT seekableChanged(d->currentSeekable);

    ++d->currentDecoderGeneration;
    d->decodePipeline.setSource(source, d->currentDecoderGeneration);
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    qCDebug(dragonsdlPlayer) << "setNextSource(" << nextSource.toString() << ")";
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline.setNextSource(nextSource, d->currentDecoderGeneration);
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

    d->requestedPlaybackState = DragonPlayer::PlaybackState::PlayingState;

    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
        return;
    }

    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
        if (d->audioOutput) {
            d->audioOutput->resume();
        }
        d->setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        qCDebug(dragonsdlPlayer) << "play() status is LoadingMedia, deferring to onFormatReady";
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::EndOfMedia) {
        qCDebug(dragonsdlPlayer) << "play() status is EndOfMedia, reloading source";
        setSource(d->currentSource);
        return;
    }

    qCDebug(dragonsdlPlayer) << "play() status is " << static_cast<int>(d->currentStatus) << ", starting audio synchronously";

    if (!d->decodePipeline.isActive() && !d->currentSource.isEmpty()) {
        qCDebug(dragonsdlPlayer) << "play() decoder not active, restarting decode pipeline";
        setSource(d->currentSource);
        return;
    }

    if (d->audioOutput && !d->audioOutput->isDeviceOpen() && d->currentSampleRate > 0) {
        d->audioOutput->start(d->currentSampleRate, d->currentChannels);
    }
    d->setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
}

void DragonPlayer::pause()
{
    qCDebug(dragonsdlPlayer) << "pause()";
    d->requestedPlaybackState = DragonPlayer::PlaybackState::PausedState;

    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        qCDebug(dragonsdlPlayer) << "pause() status is LoadingMedia, deferring to onFormatReady";
        return;
    }

    if (d->audioOutput) {
        d->audioOutput->pause();
    }
    d->setPlaybackState(DragonPlayer::PlaybackState::PausedState);
}

void DragonPlayer::stop()
{
    qCDebug(dragonsdlPlayer) << "stop()";

    d->requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        qCDebug(dragonsdlPlayer) << "stop() status is LoadingMedia, intent captured";
        return;
    }

    if (d->decodePipeline.isActive() && d->audioOutput && !d->audioOutput->isDeviceOpen()) {
        qCDebug(dragonsdlPlayer) << "stop() decoder exists but audio not open yet, ignoring stale stop";
        return;
    }

    d->decodePipeline.stopSession();
    ++d->currentDecoderGeneration;

    if (d->audioOutput) {
        d->audioOutput->stop();
        d->audioOutput->reset();
    }

    d->fftPipeline.stop();

    d->setPlaybackState(PlaybackState::StoppedState);

    if (d->currentStatus != MediaStatus::LoadedMedia) {
        d->currentStatus = MediaStatus::LoadedMedia;
    }
    Q_EMIT statusChanged(MediaStatus::LoadedMedia);
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
