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

void onFormatReady(DragonPlayerPrivate *d, int sampleRate, int channels, bool isGapless)
{
    qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels;
    d->currentSampleRate = sampleRate;
    d->currentChannels = channels;

    bool audioAlreadyRunning = isGapless && d->audioOutput->hasFormat(sampleRate, channels);

    d->audioOutput->setPositionOffset(d->currentPosition);

    d->fftPipeline.setSampleRate(sampleRate);

    if (d->currentStatus != DragonPlayer::MediaStatus::LoadedMedia) {
        d->currentStatus = DragonPlayer::MediaStatus::LoadedMedia;
        Q_EMIT d->q->statusChanged(DragonPlayer::MediaStatus::LoadedMedia);
    }

    if (d->requestedPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
        d->requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioAlreadyRunning) {
            d->audioOutput->start(sampleRate, channels);
        }

        if (d->currentPlaybackState != DragonPlayer::PlaybackState::PlayingState) {
            d->currentPlaybackState = DragonPlayer::PlaybackState::PlayingState;
            Q_EMIT d->q->playbackStateChanged(DragonPlayer::PlaybackState::PlayingState);
            Q_EMIT d->q->playing();
            if (d->positionTimer) {
                d->positionTimer->start();
            }
        }
    } else if (d->requestedPlaybackState == DragonPlayer::PlaybackState::PausedState) {
        d->requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        if (!audioAlreadyRunning) {
            d->audioOutput->start(sampleRate, channels);
        }
        if (d->audioOutput) {
            d->audioOutput->pause();
        }

        if (d->currentPlaybackState != DragonPlayer::PlaybackState::PausedState) {
            d->currentPlaybackState = DragonPlayer::PlaybackState::PausedState;
            Q_EMIT d->q->playbackStateChanged(DragonPlayer::PlaybackState::PausedState);
            Q_EMIT d->q->paused();
            if (d->positionTimer) {
                d->positionTimer->stop();
            }
        }
    } else {
        if (isGapless && d->currentPlaybackState == DragonPlayer::PlaybackState::PlayingState && !audioAlreadyRunning) {
            d->audioOutput->start(sampleRate, channels);
        }
    }
}

void onGaplessTransition(DragonPlayerPrivate *d, const QUrl &newSource)
{
    d->currentSource = newSource;
    d->nextSource.clear();
    d->currentPosition = 0;
    d->currentIsLocal = newSource.isLocalFile();
    d->currentSeekable = d->currentIsLocal;
    d->currentDuration = 0;

    d->audioOutput->setPositionOffset(0);

    Q_EMIT d->q->trackChanged();
    Q_EMIT d->q->sourceChanged();
    Q_EMIT d->q->nextSourceChanged();
    Q_EMIT d->q->seekableChanged(d->currentSeekable);
}

void onDecodeFinished(DragonPlayerPrivate *d, bool hadFatalError)
{
    if (hadFatalError) {
        QMetaObject::invokeMethod(
            d->q,
            [d]() {
                if (d->currentError != (d->currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError)) {
                    d->currentError = d->currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
                    Q_EMIT d->q->errorChanged(d->currentError);
                }
                if (d->currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
                    d->currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
                    Q_EMIT d->q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
                }
                if (d->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState) {
                    d->currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
                    Q_EMIT d->q->playbackStateChanged(DragonPlayer::PlaybackState::StoppedState);
                    Q_EMIT d->q->stopped();
                    if (d->positionTimer) {
                        d->positionTimer->stop();
                    }
                }
            },
            Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(
            d->q,
            [d]() {
                if (d->audioOutput) {
                    d->audioOutput->stop();
                    d->audioOutput->reset();
                }
                d->fftPipeline.stop();

                if (d->currentStatus != DragonPlayer::MediaStatus::EndOfMedia) {
                    d->currentStatus = DragonPlayer::MediaStatus::EndOfMedia;
                    Q_EMIT d->q->statusChanged(DragonPlayer::MediaStatus::EndOfMedia);
                }
                if (d->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState) {
                    d->currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
                    Q_EMIT d->q->playbackStateChanged(DragonPlayer::PlaybackState::StoppedState);
                    Q_EMIT d->q->stopped();
                    if (d->positionTimer) {
                        d->positionTimer->stop();
                    }
                }
            },
            Qt::QueuedConnection);
    }
}

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

void writeToQueues(DragonPlayerPrivate *d, std::span<const std::float32_t> pcm, const std::stop_token &st)
{
    if (pcm.empty()) {
        return;
    }

    writeToQueueWithBackpressure(*d->audioQueue, pcm, st);
}

void wirePipelineCallbacks(DragonPlayerPrivate *d)
{
    d->decodePipeline.setCallbacks(
        [d](int sr, int ch, bool isGapless) {
            onFormatReady(d, sr, ch, isGapless);
        },
        [d](int64_t dur) {
            if (d->currentDuration != dur) {
                d->currentDuration = dur;
                Q_EMIT d->q->durationChanged(dur);
            }
        },
        [d](auto samples, const std::stop_token &st) {
            writeToQueues(d, samples, st);
        },
        [d](const QString &) {
            if (d->currentError != (d->currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError)) {
                d->currentError = d->currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
                Q_EMIT d->q->errorChanged(d->currentError);
            }
        },
        [d](bool hadFatalError, bool) {
            onDecodeFinished(d, hadFatalError);
        },
        [d](const QUrl &newSource) {
            onGaplessTransition(d, newSource);
        });
}

void wireFftCallbacks(DragonPlayerPrivate *d)
{
    d->fftPipeline.setFrameCallback([d](DragonFftFrame frame) {
        QMetaObject::invokeMethod(
            d->q,
            [d, f = std::move(frame)]() mutable {
                Q_EMIT d->q->fftFrameReady(f);
            },
            Qt::QueuedConnection);
    });
}

void setPlaybackState(DragonPlayerPrivate *d, DragonPlayer::PlaybackState state)
{
    if (d->currentPlaybackState == state) {
        return;
    }

    if (state == DragonPlayer::PlaybackState::PlayingState) {
        if (d->positionTimer) {
            d->positionTimer->start();
        }
    } else {
        if (d->positionTimer) {
            d->positionTimer->stop();
        }
    }

    d->currentPlaybackState = state;

    Q_EMIT d->q->playbackStateChanged(state);

    switch (state) {
    case DragonPlayer::PlaybackState::PlayingState:
        Q_EMIT d->q->playing();
        break;
    case DragonPlayer::PlaybackState::PausedState:
        Q_EMIT d->q->paused();
        break;
    case DragonPlayer::PlaybackState::StoppedState:
        Q_EMIT d->q->stopped();
        break;
    default:
        std::unreachable();
    }
}

void setStatus(DragonPlayerPrivate *d, DragonPlayer::MediaStatus status)
{
    if (d->currentStatus == status) {
        return;
    }
    d->currentStatus = status;
    Q_EMIT d->q->statusChanged(status);

    if (status == DragonPlayer::MediaStatus::InvalidMedia && d->currentError != DragonPlayer::Error::FormatError) {
        d->currentError = DragonPlayer::Error::FormatError;
        Q_EMIT d->q->errorChanged(d->currentError);
    }
}

void setError(DragonPlayerPrivate *d, DragonPlayer::Error error)
{
    if (d->currentError == error) {
        return;
    }
    d->currentError = error;
    Q_EMIT d->q->errorChanged(error);

    if (error != DragonPlayer::Error::NoError && d->currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
        d->currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
        Q_EMIT d->q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
    }
}

void stopPipeline(DragonPlayerPrivate *d)
{
    qCDebug(dragonsdlPlayer) << "stopPipeline() full teardown";

    d->decodePipeline.stop();

    d->fftPipeline.stop();

    if (d->audioOutput) {
        d->audioOutput->stop();
        d->audioOutput->reset();
    }

    qCDebug(dragonsdlPlayer) << "stopPipeline() teardown complete";
}

void initPrivate(DragonPlayerPrivate *d)
{
    d->audioBuffer.resize(DragonPlayerPrivate::kBufferCapacity);
    d->audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(d->audioBuffer));

    d->audioOutput = std::make_unique<DragonAudioOutput>();
    d->audioOutput->setQueue(d->audioQueue.get());
    d->audioOutput->setFftQueue(nullptr);

    QObject::connect(d->audioOutput.get(), &DragonAudioOutput::errorOccurred, d->q, [d](const QString &) {
        setError(d, DragonPlayer::Error::ResourceError);
    });

    QObject::connect(d->audioOutput.get(), &DragonAudioOutput::volumeChanged, d->q, &DragonPlayer::volumeChanged);

    d->positionTimer = new QTimer(d->q);
    d->positionTimer->setInterval(100);
    QObject::connect(d->positionTimer, &QTimer::timeout, d->q, [d]() {
        Q_EMIT d->q->positionChanged(d->audioOutput && d->audioOutput->isDeviceOpen() ? d->audioOutput->positionMs() : d->currentPosition);
    });

    wirePipelineCallbacks(d);
    wireFftCallbacks(d);
}

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
    initPrivate(d.get());
}

DragonPlayer::~DragonPlayer()
{
    stopPipeline(d.get());
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
    return d->audioOutput ? d->audioOutput->isDeviceOpen() : false;
}
DragonPlayer::FftMode DragonPlayer::fftMode() const
{
    return d->currentFftMode;
}

void DragonPlayer::setMuted(bool muted)
{
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
        setStatus(d.get(), DragonPlayer::MediaStatus::NoMedia);
        setPlaybackState(d.get(), DragonPlayer::PlaybackState::StoppedState);
        return;
    }

    if (d->currentPlaybackState != DragonPlayer::PlaybackState::StoppedState) {
        setPlaybackState(d.get(), DragonPlayer::PlaybackState::StoppedState);
    } else {
        Q_EMIT playbackStateChanged(DragonPlayer::PlaybackState::StoppedState);
        Q_EMIT stopped();
    }

    d->requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;

    if (d->currentError != DragonPlayer::Error::NoError) {
        d->currentError = DragonPlayer::Error::NoError;
        Q_EMIT errorChanged(DragonPlayer::Error::NoError);
    }

    setStatus(d.get(), DragonPlayer::MediaStatus::LoadingMedia);

    const bool isLocal = source.isLocalFile();
    d->currentIsLocal = isLocal;
    d->currentSeekable = isLocal;
    Q_EMIT seekableChanged(d->currentSeekable);

    ++d->currentDecoderGeneration;
    d->decodePipeline.setSource(source, d->currentDecoderGeneration);
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline.setNextSource(nextSource, d->currentDecoderGeneration);
}

void DragonPlayer::setPosition(int64_t posMs)
{
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
    if (d->currentSource.isEmpty()) {
        return;
    }

    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
        return;
    }

    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
        if (d->audioOutput) {
            d->audioOutput->resume();
        }
        setPlaybackState(d.get(), DragonPlayer::PlaybackState::PlayingState);
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        d->requestedPlaybackState = DragonPlayer::PlaybackState::PlayingState;
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::EndOfMedia) {
        setSource(d->currentSource);
        d->requestedPlaybackState = DragonPlayer::PlaybackState::PlayingState;
        return;
    }

    if (d->audioOutput && !d->audioOutput->isDeviceOpen() && d->currentSampleRate > 0) {
        d->audioOutput->start(d->currentSampleRate, d->currentChannels);
    }
    setPlaybackState(d.get(), DragonPlayer::PlaybackState::PlayingState);
}

void DragonPlayer::pause()
{
    if (d->currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
        return;
    }

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        d->requestedPlaybackState = DragonPlayer::PlaybackState::PausedState;
        return;
    }

    if (d->audioOutput) {
        d->audioOutput->pause();
    }
    setPlaybackState(d.get(), DragonPlayer::PlaybackState::PausedState);
}

void DragonPlayer::stop()
{
    qCDebug(dragonsdlPlayer) << "stop()";

    if (d->currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
        d->requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
        return;
    }

    if (d->decodePipeline.isActive() && d->audioOutput && !d->audioOutput->isDeviceOpen()) {
        qCDebug(dragonsdlPlayer) << "stop() decoder exists but audio not open yet, ignoring stale stop";
        return;
    }

    d->decodePipeline.stop();
    ++d->currentDecoderGeneration;

    if (d->audioOutput) {
        d->audioOutput->stop();
        d->audioOutput->reset();
    }

    d->fftPipeline.stop();

    setPlaybackState(d.get(), DragonPlayer::PlaybackState::StoppedState);

    if (d->currentStatus != DragonPlayer::MediaStatus::LoadedMedia) {
        d->currentStatus = DragonPlayer::MediaStatus::LoadedMedia;
    }
    Q_EMIT statusChanged(DragonPlayer::MediaStatus::LoadedMedia);
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
