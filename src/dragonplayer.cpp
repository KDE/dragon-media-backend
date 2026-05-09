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

namespace
{
size_t writeToQueueWithBackpressure(LockFreeSpscQueue<std::float32_t> &queue, std::span<const std::float32_t> pcm, const std::stop_token &st)
{
    size_t written = 0;
    while (written < pcm.size() && !st.stop_requested()) {
        auto remaining = pcm.subspan(written);
        size_t n = queue.try_write(remaining.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
            auto in_iter = std::ranges::copy_n(remaining.begin(), b1.size(), b1.begin()).in;
            std::ranges::copy_n(in_iter, b2.size(), b2.begin());
        });
        written += n;
        if (written < pcm.size() && !st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    return written;
}
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels)
{
    switch (requestedPlaybackState) {
    case DragonPlayer::PlaybackState::PlayingState:
        requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioOutput->isDeviceOpen()) {
            audioOutput->start(sampleRate, channels);
        }
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        break;

    case DragonPlayer::PlaybackState::PausedState:
        requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioOutput->isDeviceOpen()) {
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

    audioOutput->setPositionOffset(currentPosition);
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

    audioOutput->setPositionOffset(0);

    Q_EMIT q->trackChanged();
    Q_EMIT q->sourceChanged();
    Q_EMIT q->nextSourceChanged();
    Q_EMIT q->seekableChanged(currentSeekable);
}

void DragonPlayerPrivate::onDecodeFinished(bool hadFatalError)
{
    if (hadFatalError) {
        QMetaObject::invokeMethod(
            q,
            [this]() {
                if (currentError != (currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError)) {
                    currentError = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
                    Q_EMIT q->errorChanged(currentError);
                }
                if (currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
                    currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
                    Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
                }
                setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
            },
            Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(
            q,
            [this]() {
                if (audioOutput && audioOutput->isDeviceOpen()) {
                    audioOutput->pause();
                }

                fftPipeline.stop();

                if (currentStatus != DragonPlayer::MediaStatus::EndOfMedia) {
                    currentStatus = DragonPlayer::MediaStatus::EndOfMedia;
                    Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::EndOfMedia);
                }
                setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
            },
            Qt::QueuedConnection);
    }
}

void DragonPlayerPrivate::writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st)
{
    if (pcm.empty()) {
        return;
    }

    writeToQueueWithBackpressure(*audioQueue, pcm, st);
}

void DragonPlayerPrivate::wirePipelineCallbacks()
{
    decodePipeline.setCallbacks(
        [this](int sr, int ch, bool isGapless) {
            onFormatReady(sr, ch, isGapless);
        },
        [this](int64_t dur) {
            if (currentDuration != dur) {
                currentDuration = dur;
                Q_EMIT q->durationChanged(dur);
            }
        },
        [this](auto samples, const std::stop_token &st) {
            writeToQueues(samples, st);
        },
        [this](const QString &) {
            if (currentError != (currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError)) {
                currentError = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
                Q_EMIT q->errorChanged(currentError);
            }
        },
        [this](bool hadFatalError, bool) {
            onDecodeFinished(hadFatalError);
        },
        [this](const QUrl &newSource) {
            onGaplessTransition(newSource);
        });
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
    audioBuffer.resize(kBufferCapacity);
    audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(audioBuffer));

    audioOutput = std::make_unique<DragonAudioOutput>();
    audioOutput->setQueue(audioQueue.get());
    audioOutput->setFftQueue(nullptr);

    QObject::connect(audioOutput.get(), &DragonAudioOutput::errorOccurred, q, [this](const QString &) {
        setError(DragonPlayer::Error::ResourceError);
    });

    QObject::connect(audioOutput.get(), &DragonAudioOutput::volumeChanged, q, &DragonPlayer::volumeChanged);

    positionTimer = new QTimer(q);
    positionTimer->setInterval(100);
    QObject::connect(positionTimer, &QTimer::timeout, q, [this]() {
        Q_EMIT q->positionChanged(audioOutput && audioOutput->isDeviceOpen() ? audioOutput->positionMs() : currentPosition);
    });

    wirePipelineCallbacks();
    wireFftCallbacks();
}

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
{
    d.reset(new DragonPlayerPrivate{.q = this,
                                    .decodePipeline = DragonDecodePipeline(this),
                                    .fftPipeline = DragonFftPipeline(),
                                    .fftBuffer = {},
                                    .audioBuffer = {},
                                    .fftQueue = nullptr,
                                    .audioQueue = nullptr,
                                    .audioOutput = nullptr,
                                    .currentSource = {},
                                    .nextSource = {},
                                    .currentPlaybackState = DragonPlayer::PlaybackState::StoppedState,
                                    .currentStatus = DragonPlayer::MediaStatus::NoMedia,
                                    .currentError = DragonPlayer::Error::NoError,
                                    .requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState,
                                    .currentDuration = 0,
                                    .currentVolume = 1.0f,
                                    .currentMuted = false,
                                    .currentSeekable = false,
                                    .currentIsLocal = false,
                                    .currentSampleRate = 0,
                                    .currentChannels = 0,
                                    .currentFftMode = DragonPlayer::FftMode::Off,
                                    .currentDecoderGeneration = 0,
                                    .undoPosition = 0,
                                    .positionTimer = nullptr,
                                    .currentPosition = 0});
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
        d->audioOutput->stop();
        d->audioOutput->reset();
    }

    if (d->currentFftMode == DragonPlayer::FftMode::Off) {
        d->fftPipeline.stop();
        d->fftPipeline.teardown();
        d->fftQueue.reset();
        d->fftBuffer.clear();
        d->fftBuffer.shrink_to_fit();
        if (d->audioOutput) {
            d->audioOutput->setFftQueue(nullptr);
        }
    } else {
        d->fftPipeline.ensureInfrastructure(&d->fftBuffer, d->audioOutput->fftCv(), d->currentFftMode);

        d->fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(d->fftBuffer));
        if (d->audioOutput) {
            d->audioOutput->setFftQueue(d->fftQueue.get());
        }

        d->fftPipeline.restartWithNewQueue(d->fftQueue.get(), d->audioOutput->fftCv());
    }

    d->audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(d->audioBuffer));
    d->audioOutput->setQueue(d->audioQueue.get());

    d->currentSource = source;
    d->currentPosition = 0;
    d->currentDuration = 0;
    d->nextSource.clear();
    d->currentSampleRate = 0;
    d->currentChannels = 0;

    Q_EMIT sourceChanged();
    Q_EMIT nextSourceChanged();

    if (source.isEmpty()) {
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
        d->audioOutput->setPositionOffset(posMs);
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
        d->fftPipeline.ensureInfrastructure(&d->fftBuffer, d->audioOutput->fftCv(), mode);

        if (!d->fftQueue) {
            d->fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(d->fftBuffer));
        }

        if (d->audioOutput) {
            d->audioOutput->setFftQueue(d->fftQueue.get());
        }

        d->fftPipeline.setQueue(d->fftQueue.get());

        if (d->fftPipeline.isRunning()) {
            d->fftPipeline.restartWithNewQueue(d->fftQueue.get(), d->audioOutput->fftCv());
        } else {
            d->fftPipeline.start();
        }
    } else if (wasOn && !nowOn) {
        if (d->audioOutput) {
            d->audioOutput->setFftQueue(nullptr);
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
