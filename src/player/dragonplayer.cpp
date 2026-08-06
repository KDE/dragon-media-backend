/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"

#include "sink/dragonaudiosinkfactory.h"
#include <DragonMultimedia/dragonplayer.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#include <QCoroTask>
#pragma GCC diagnostic pop
#include <KLocalizedString>
#include <QMetaObject>
#include <QTimer>
#include <dragonmultimedia_logging.h>

#include "dragonstdfloat_compat.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ranges>
#include <stop_token>
#include <thread>
#include <utility>

DragonPlayerPrivate::DragonPlayerPrivate(DragonPlayer *player, DragonPlayer::AudioSink requestedSink)
    : QObject(player)
    , q(player)
    , decodePipeline(player)
    , requestedAudioSink(requestedSink)
    , aliveGuard(std::make_shared<AliveGuard>())
{
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent)
{
    switch (intent) {
    case DragonPlayer::PlaybackState::PlayingState:
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->open(sampleRate, channels);
        } else {
            audioOutput->resume();
        }
        audioOutput->setQueueReady(true);
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        break;

    case DragonPlayer::PlaybackState::PausedState:
        if (!audioOutput->isDeviceOpen() || !audioOutput->hasFormat(sampleRate, channels)) {
            audioOutput->open(sampleRate, channels);
        }
        audioOutput->pause();
        audioOutput->setQueueReady(true);
        setPlaybackState(DragonPlayer::PlaybackState::PausedState);
        break;

    case DragonPlayer::PlaybackState::StoppedState:
        break;
    }
}

void DragonPlayerPrivate::onGaplessTransition(const QUrl &newSource, int sampleRate, int channels, qint64 durationMs)
{
    qCDebug(dragonMultimediaPlayer) << "onGaplessTransition newSource=" << newSource.toString() << "sr=" << sampleRate << "ch=" << channels
                                    << "duration=" << durationMs;

    if (newSource != nextSource) {
        qCDebug(dragonMultimediaPlayer) << "ignoring stale onGaplessTransition (source mismatch)";
        return;
    }

    if (nextSource.isEmpty()) {
        qCDebug(dragonMultimediaPlayer) << "onGaplessTransition nextSource was cleared, aborting";
        return;
    }

    currentSource = nextSource;
    nextSource.clear();
    currentPosition = 0;
    aboutToFinishEmitted = false;
    qCDebug(dragonMultimediaPlayer) << "onGaplessTransition: reset aboutToFinishEmitted for" << currentSource.toString();
    currentIsLocal = currentSource.isLocalFile();
    currentSeekable = currentIsLocal;
    currentDuration = durationMs;
    currentSampleRate = sampleRate;
    currentChannels = channels;

    decodePipeline.setCurrentSource(currentSource);

    audioOutput->setPositionOffset(0, DragonAudioSink::PositionResetMode::GaplessTransition);
    Q_EMIT q->positionChanged(0);

    Q_EMIT q->trackChanged();
    Q_EMIT q->sourceChanged();
    Q_EMIT q->nextSourceChanged();
    Q_EMIT q->seekableChanged(currentSeekable);

    if (currentDuration >= 0) {
        Q_EMIT q->durationChanged(currentDuration);
    }

    inGaplessSetSource = true;
}

void DragonPlayerPrivate::onDecodeFinished(const QUrl &source, bool hadFatalError)
{
    qCDebug(dragonMultimediaPlayer) << "onDecodeFinished called source=" << source.toString() << "currentSource=" << currentSource.toString()
                                    << "hadFatalError=" << hadFatalError;

    if (source != currentSource) {
        qCDebug(dragonMultimediaPlayer) << "ignoring stale onDecodeFinished (source mismatch)";
        return;
    }

    if (hadFatalError) {
        if (audioOutput) {
            audioOutput->close();
            audioOutput->setQueueReady(false);
        }
        setStatus(DragonPlayer::MediaStatus::InvalidMedia);
        if (currentError != DragonPlayer::Error::NoError && currentErrorString.isEmpty()) {
            currentErrorString = i18n("Fatal error while decoding media");
            Q_EMIT q->errorChanged(currentError);
        }
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        qCDebug(dragonMultimediaPlayer) << "onDecodeFinished state/status changes complete (fatal)";
        return;
    }

    if (!nextSource.isEmpty()) {
        // Gapless handoff: don't close the device the next track's audio
        // is already flowing into the pipe and the PA callback will consume
        // it seamlessly without a gap.
        q->setSource(nextSource);
        return;
    }

    // normal end of track with no gapless handoff: mark EndOfMedia and
    // Normal end of track with no gapless handoff: emit aboutToFinish
    // here if all audio has been queued but positionMs() hasn't caught up
    // yet (the ~200ms PA device buffer + pipe backlog).  Defer
    // EndOfMedia/StoppedState until the backend signals drained().
    if (prefinishMark > 0 && !aboutToFinishEmitted && audioOutput && currentDuration > 0) {
        const qint64 pipeBacklog = static_cast<qint64>(audioPipe.consumer().ready());
        const qint64 eventualSamples = audioOutput->totalSamplesWritten() + pipeBacklog;
        const qint64 eventualPositionMs = (eventualSamples / currentChannels) * 1000 / currentSampleRate;
        const qint64 remaining = currentDuration - eventualPositionMs;
        if (remaining <= prefinishMark) {
            aboutToFinishEmitted = true;
            qCDebug(dragonMultimediaPlayer) << "aboutToFinish emitted from onDecodeFinished"
                                            << "eventualPos=" << eventualPositionMs << "remaining=" << remaining;
            Q_EMIT q->aboutToFinish();
        }
    }
    audioOutput->notifyDecodeFinished();
    qCDebug(dragonMultimediaPlayer) << "onDecodeFinished media ended, deferring StoppedState until drain";
}

void DragonPlayerPrivate::onDecodeError(const QString &message)
{
    auto err = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
    currentErrorString = !message.isEmpty() ? message : i18n("Error while decoding media");
    setStatus(DragonPlayer::MediaStatus::InvalidMedia);
    if (currentError != err) {
        currentError = err;
    }
    Q_EMIT q->errorChanged(currentError);
}

void DragonPlayerPrivate::onStreamStalled()
{
    setStatus(DragonPlayer::MediaStatus::StalledMedia);
}

void DragonPlayerPrivate::onStreamBuffering()
{
    setStatus(DragonPlayer::MediaStatus::BufferingMedia);
}

void DragonPlayerPrivate::onStreamBuffered()
{
    setStatus(DragonPlayer::MediaStatus::BufferedMedia);
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
    qCDebug(dragonMultimediaPlayer) << "setPlaybackState(" << state << ") current=" << currentPlaybackState;
    if (currentPlaybackState == state) {
        qCDebug(dragonMultimediaPlayer) << "setPlaybackState no change, returning";
        return;
    }

    const auto oldState = currentPlaybackState;

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

    Q_EMIT q->stateChanged(state, oldState);
}

void DragonPlayerPrivate::setStatus(DragonPlayer::MediaStatus status)
{
    qCDebug(dragonMultimediaPlayer) << "setStatus(" << status << ") current=" << currentStatus;
    if (currentStatus == status) {
        qCDebug(dragonMultimediaPlayer) << "setStatus no change, returning";
        return;
    }
    currentStatus = status;
    qCDebug(dragonMultimediaPlayer) << "setStatus emitting statusChanged";
    Q_EMIT q->statusChanged(status);

    if ((status == DragonPlayer::MediaStatus::NoMedia || status == DragonPlayer::MediaStatus::LoadedMedia) && !currentErrorString.isEmpty()) {
        currentErrorString.clear();
        Q_EMIT q->errorChanged(currentError);
    }
}

void DragonPlayerPrivate::setError(DragonPlayer::Error error, const QString &message)
{
    if (error == DragonPlayer::Error::NoError) {
        currentErrorString.clear();
        if (currentError == error) {
            return;
        }
        currentError = error;
        Q_EMIT q->errorChanged(error);
        return;
    }

    currentErrorString = message;

    if (currentError == error) {
        if (!message.isEmpty()) {
            Q_EMIT q->errorChanged(error);
        }
        return;
    }
    currentError = error;
    Q_EMIT q->errorChanged(error);

    if (currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
        currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
    }
}

void DragonPlayerPrivate::stopPipeline()
{
    qCDebug(dragonMultimediaPlayer) << "stopPipeline() full teardown";

    decodePipeline.stop();

    fftPipeline.stop();

    if (audioOutput) {
        audioOutput->close();
        audioOutput->reset();
    }

    qCDebug(dragonMultimediaPlayer) << "stopPipeline() teardown complete";
}

void DragonPlayerPrivate::init()
{
    audioOutput = createAudioSink(requestedAudioSink, &selectedAudioSink);
    if (audioOutput) {
        audioOutput->setAudioPipe(&audioPipe);
        audioOutput->setFftPipe(nullptr);
    }

    connect(audioOutput.get(), &DragonAudioSink::errorOccurred, this, [this](const QString &message) {
        setError(DragonPlayer::Error::ResourceError, message);
    });

    connect(audioOutput.get(), &DragonAudioSink::volumeChanged, q, &DragonPlayer::volumeChanged);

    connect(audioOutput.get(), &DragonAudioSink::drained, this, [this]() {
        setStatus(DragonPlayer::MediaStatus::EndOfMedia);
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
    });

    positionTimer = new QTimer(this);
    positionTimer->setInterval(100);
    connect(positionTimer, &QTimer::timeout, this, [this]() {
        const qint64 pos = audioOutput && audioOutput->isDeviceOpen() ? audioOutput->positionMs() : currentPosition;
        Q_EMIT q->positionChanged(pos);

        if (prefinishMark > 0) {
            qCDebug(dragonMultimediaPlayer) << "timer: prefinishMark=" << prefinishMark << "currentDuration=" << currentDuration
                                            << "emitted=" << aboutToFinishEmitted << "pos=" << pos;
            if (currentDuration > 0 && !aboutToFinishEmitted) {
                const qint64 remaining = currentDuration - pos;
                qCDebug(dragonMultimediaPlayer) << "timer: remaining=" << remaining;
                if (remaining <= prefinishMark && remaining > 0) {
                    aboutToFinishEmitted = true;
                    qCDebug(dragonMultimediaPlayer) << "aboutToFinish emitted remaining=" << remaining << "ms, prefinishMark=" << prefinishMark;
                    Q_EMIT q->aboutToFinish();
                }
            }
        }
    });

    decodePipeline.setSamplesCallback([this](auto samples, const std::stop_token &st) {
        writeToQueues(samples, st);
    });

    connect(&decodePipeline, &DragonDecodePipeline::sessionError, this, &DragonPlayerPrivate::onDecodeError, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::sessionFinished, this, &DragonPlayerPrivate::onDecodeFinished, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::gaplessTransition, this, &DragonPlayerPrivate::onGaplessTransition, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::streamStalled, this, &DragonPlayerPrivate::onStreamStalled, Qt::QueuedConnection);
    connect(&decodePipeline, &DragonDecodePipeline::streamBuffering, this, &DragonPlayerPrivate::onStreamBuffering, Qt::QueuedConnection);
    connect(&decodePipeline, &DragonDecodePipeline::streamBuffered, this, &DragonPlayerPrivate::onStreamBuffered, Qt::QueuedConnection);

    connect(
        &decodePipeline,
        &DragonDecodePipeline::bufferProgressChanged,
        this,
        [this](qreal progress) {
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

DragonPlayer::DragonPlayer(AudioSink requestedSink, QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DragonFftFrame>();

    d = std::make_unique<DragonPlayerPrivate>(this, requestedSink);
    d->init();

    connect(this, &DragonPlayer::stateChanged, this, [](PlaybackState newState, PlaybackState oldState) {
        qCDebug(dragonMultimediaPlayer) << "playbackState changed from" << oldState << "to" << newState;
    });
    connect(this, &DragonPlayer::statusChanged, this, [](MediaStatus status) {
        qCDebug(dragonMultimediaPlayer) << "mediaStatus changed to" << status;
    });
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
qreal DragonPlayer::volume() const
{
    return d->audioOutput ? d->audioOutput->volume() : 1.0;
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
QString DragonPlayer::errorString() const
{
    return d->currentErrorString;
}
qint64 DragonPlayer::duration() const
{
    return d->currentDuration;
}
qint64 DragonPlayer::position() const
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
DragonPlayer::FftMode DragonPlayer::fftMode() const
{
    return d->currentFftMode;
}
int DragonPlayer::fftRate() const
{
    return d->currentFftRate;
}
qreal DragonPlayer::bufferProgress() const
{
    return d->currentBufferProgress;
}
DragonPlayer::AudioSink DragonPlayer::selectedAudioSink() const
{
    return d->selectedAudioSink;
}

void DragonPlayer::setMuted(bool muted)
{
    qCDebug(dragonMultimediaPlayer) << "setMuted(" << muted << ")";
    if (d->currentMuted == muted) {
        return;
    }
    d->currentMuted = muted;
    if (d->audioOutput) {
        d->audioOutput->setMuted(muted);
    }
    Q_EMIT d->q->mutedChanged(muted);
}

void DragonPlayer::setVolume(qreal gain)
{
    qCDebug(dragonMultimediaPlayer) << "setVolume(" << gain << ")";
    if (d->audioOutput) {
        d->audioOutput->setVolume(static_cast<float>(gain));
    }
}

QCoro::Task<void> DragonPlayerPrivate::startLoad(QUrl source, uint64_t generation)
{
    auto aliveGuard = this->aliveGuard;

    auto result = co_await decodePipeline.initializeSession(source);

    if (!aliveGuard || !aliveGuard->alive) {
        co_return;
    }

    if (generation != loadGeneration) {
        qCDebug(dragonMultimediaPlayer) << "startLoad superseded (generation" << generation << "!= current" << loadGeneration << "), discarding";
        co_return;
    }

    if (!result.success) {
        if (result.cancelled) {
            qCDebug(dragonMultimediaPlayer) << "startLoad cancelled, returning without state change";
            co_return;
        }
        setStatus(DragonPlayer::MediaStatus::InvalidMedia);
        currentError = currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError;
        currentErrorString =
            result.errorMessage.isEmpty() ? (currentIsLocal ? i18n("Failed to open media source") : i18n("Network error")) : result.errorMessage;
        Q_EMIT q->errorChanged(currentError);
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        co_return;
    }

    currentSampleRate = result.sampleRate;
    currentChannels = result.channels;
    currentDuration = result.durationMs;

    if (currentError != DragonPlayer::Error::NoError || !currentErrorString.isEmpty()) {
        currentError = DragonPlayer::Error::NoError;
        currentErrorString.clear();
        Q_EMIT q->errorChanged(DragonPlayer::Error::NoError);
    }

    if (currentDuration >= 0) {
        Q_EMIT q->durationChanged(currentDuration);
    }

    fftPipeline.setSampleRate(result.sampleRate);
    fftPipeline.setChannelCount(result.channels);

    if (!currentIsLocal && decodePipeline.streamSize() > 0) {
        currentSeekable = true;
        Q_EMIT q->seekableChanged(true);
    }

    setStatus(DragonPlayer::MediaStatus::LoadedMedia);

    applyRequestedState(result.sampleRate, result.channels, requestedPlaybackState);

    co_return;
}

void DragonPlayer::setSource(const QUrl &source)
{
    qCDebug(dragonMultimediaPlayer) << "setSource(" << source.toString() << ")";

    bool playRequestedReload = d->playRequestedReload;
    d->playRequestedReload = false;

    if (!playRequestedReload && d->currentSource == source && source.isValid()) {
        qCDebug(dragonMultimediaPlayer) << "setSource(sameUrl) early return";
        if (d->currentStatus == MediaStatus::LoadingMedia) {
            d->requestedPlaybackState = PlaybackState::StoppedState;
            return; // Let the existing initialization finish
        }

        d->requestedPlaybackState = PlaybackState::StoppedState;
        stop();
        return;
    }

    if (d->audioOutput) {
        d->audioOutput->setQueueReady(false);
        d->audioOutput->resetDrainState();
        d->audioOutput->setPositionOffset(0, DragonAudioSink::PositionResetMode::NormalTrackChange);
    }

    d->decodePipeline.stopSession();

    d->audioOutput->setFftPipe(d->currentFftMode != FftMode::Off ? &d->fftPipe : nullptr);
    d->fftPipeline.restart();

    const bool isGapless = d->inGaplessSetSource;
    d->inGaplessSetSource = false;

    const bool wasPlayingOrPaused = d->currentPlaybackState == PlaybackState::PlayingState || d->currentPlaybackState == PlaybackState::PausedState;

    const bool hadValidMedia = d->currentStatus == MediaStatus::LoadedMedia || d->currentStatus == MediaStatus::BufferedMedia
        || d->currentStatus == MediaStatus::StalledMedia || d->currentStatus == MediaStatus::BufferingMedia || d->currentStatus == MediaStatus::EndOfMedia;

    d->currentSource = source;
    d->currentPosition = 0;
    d->currentDuration = 0;
    d->aboutToFinishEmitted = false;
    qCDebug(dragonMultimediaPlayer) << "setSource: reset aboutToFinishEmitted for" << d->currentSource.toString();
    d->nextSource.clear();
    d->currentSampleRate = 0;
    d->currentChannels = 0;

    if (!isGapless && !playRequestedReload) {
        d->requestedPlaybackState = PlaybackState::StoppedState;
        if (wasPlayingOrPaused) {
            d->setPlaybackState(PlaybackState::StoppedState);
        }
        if (hadValidMedia || d->currentStatus == MediaStatus::InvalidMedia) {
            Q_EMIT statusChanged(MediaStatus::LoadedMedia);
        }

        if (d->currentSource.isEmpty()) {
            if (d->audioOutput) {
                d->audioOutput->close();
                d->audioOutput->reset();
            }
            if (d->currentError != Error::NoError || !d->currentErrorString.isEmpty()) {
                d->currentError = Error::NoError;
                d->currentErrorString.clear();
                Q_EMIT errorChanged(Error::NoError);
            }
            d->setStatus(MediaStatus::NoMedia);
            Q_EMIT nextSourceChanged();
            Q_EMIT sourceChanged();
            return;
        }

        if (d->currentError != Error::NoError || !d->currentErrorString.isEmpty()) {
            d->currentError = Error::NoError;
            d->currentErrorString.clear();
            Q_EMIT errorChanged(Error::NoError);
        }

        d->setStatus(MediaStatus::LoadingMedia);

        Q_EMIT nextSourceChanged();
        Q_EMIT sourceChanged();
    } else if (playRequestedReload && d->requestedPlaybackState == PlaybackState::PlayingState) {
        // Internal reload initiated by play() at EndOfMedia or after a decoder
        // restart: keep the Playing intent so applyRequestedState starts audio.
        qCDebug(dragonMultimediaPlayer) << "setSource internal reload preserving Playing intent";
        d->setStatus(MediaStatus::LoadingMedia);
    }

    const bool isLocal = d->currentSource.isLocalFile();
    const bool isHttp = d->currentSource.scheme() == QStringLiteral("http") || d->currentSource.scheme() == QStringLiteral("https");

    d->currentIsLocal = isLocal;
    d->currentSeekable = !isHttp;
    Q_EMIT seekableChanged(d->currentSeekable);

    ++d->loadGeneration;
    d->startLoad(d->currentSource, d->loadGeneration);
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    qCDebug(dragonMultimediaPlayer) << "setNextSource(" << nextSource.toString() << ")";
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline.setNextSource(nextSource);
}

void DragonPlayer::setPosition(qint64 posMs)
{
    qCDebug(dragonMultimediaPlayer) << "setPosition(" << posMs << ")";
    posMs = std::clamp(posMs, qint64{0}, std::max(d->currentDuration, qint64{0}));
    d->currentPosition = posMs;

    d->decodePipeline.requestSeek(posMs);

    if (d->audioOutput) {
        d->audioOutput->setPositionOffset(posMs, DragonAudioSink::PositionResetMode::Seek);
        d->audioOutput->clearStream();
    }
    Q_EMIT positionChanged(posMs);

    if (d->prefinishMark > 0 && d->currentDuration > 0) {
        const qint64 remaining = d->currentDuration - posMs;
        if (remaining > d->prefinishMark) {
            d->aboutToFinishEmitted = false;
        }
    }

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        d->currentStatus = MediaStatus::LoadedMedia;
        Q_EMIT statusChanged(MediaStatus::LoadedMedia);
    }
}

int32_t DragonPlayer::prefinishMark() const
{
    return d->prefinishMark;
}

void DragonPlayer::setPrefinishMark(int32_t msec)
{
    qCDebug(dragonMultimediaPlayer) << "setPrefinishMark(" << msec << ")";
    if (d->prefinishMark == msec) {
        return;
    }
    d->prefinishMark = msec;
    Q_EMIT prefinishMarkChanged(msec);

    if (d->prefinishMark > 0 && d->currentDuration > 0) {
        const qint64 remaining = d->currentDuration - position();
        if (remaining > d->prefinishMark) {
            d->aboutToFinishEmitted = false;
        }
    }
}

void DragonPlayer::setFftMode(FftMode mode)
{
    qCDebug(dragonMultimediaPlayer) << "setFftMode(" << mode << ")";
    if (d->currentFftMode == mode) {
        return;
    }

    d->currentFftMode = mode;
    d->audioOutput->setFftPipe(mode != FftMode::Off ? &d->fftPipe : nullptr);
    d->fftPipeline.setMode(mode);

    Q_EMIT fftModeChanged(mode);
}

void DragonPlayer::setFftRate(int rate)
{
    qCDebug(dragonMultimediaPlayer) << "setFftRate(" << rate << ")";
    if (rate <= 0) {
        rate = 1;
    }
    if (d->currentFftRate == rate) {
        return;
    }
    d->currentFftRate = rate;
    d->fftPipeline.setFftRate(rate);
    Q_EMIT fftRateChanged(rate);
}

void DragonPlayer::play()
{
    qCDebug(dragonMultimediaPlayer) << "play()";
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
        qCDebug(dragonMultimediaPlayer) << "play() status is LoadingMedia, intent captured";
        return;
    }

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        qCDebug(dragonMultimediaPlayer) << "play() status is EndOfMedia, reloading source";
        d->playRequestedReload = true;
        d->requestedPlaybackState = PlaybackState::PlayingState;
        setSource(d->currentSource);
        return;
    }

    if (!d->decodePipeline.isActive() && !d->currentSource.isEmpty()) {
        qCDebug(dragonMultimediaPlayer) << "play() decoder not active, restarting decode pipeline";
        d->playRequestedReload = true;
        d->requestedPlaybackState = PlaybackState::PlayingState;
        setSource(d->currentSource);
        return;
    }

    qCDebug(dragonMultimediaPlayer) << "play() status is " << d->currentStatus << ", starting audio synchronously";

    if (d->audioOutput && !d->audioOutput->isDeviceOpen() && d->currentSampleRate > 0) {
        d->audioOutput->open(d->currentSampleRate, d->currentChannels);
    }
    if (d->audioOutput) {
        d->audioOutput->setQueueReady(true);
    }
    d->setPlaybackState(PlaybackState::PlayingState);
}

void DragonPlayer::pause()
{
    qCDebug(dragonMultimediaPlayer) << "pause()";

    if (d->currentStatus == MediaStatus::NoMedia || d->currentStatus == MediaStatus::InvalidMedia) {
        return;
    }

    d->requestedPlaybackState = PlaybackState::PausedState;

    if (d->currentPlaybackState == PlaybackState::PausedState) {
        return;
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonMultimediaPlayer) << "pause() status is LoadingMedia, intent captured";
        return;
    }

    if (d->audioOutput) {
        d->audioOutput->pause();
    }
    d->setPlaybackState(PlaybackState::PausedState);
}

void DragonPlayer::stop()
{
    qCDebug(dragonMultimediaPlayer) << "stop()";

    d->requestedPlaybackState = PlaybackState::StoppedState;

    d->decodePipeline.stopSession();

    if (d->audioOutput) {
        d->audioOutput->close();
        d->audioOutput->reset();
        d->audioOutput->resetDrainState();
    }
    d->fftPipeline.stop();

    d->setPlaybackState(PlaybackState::StoppedState);

    if (d->currentStatus != MediaStatus::NoMedia && d->currentStatus != MediaStatus::InvalidMedia) {
        d->setStatus(MediaStatus::LoadedMedia);
    }
}

void DragonPlayer::seek(qint64 posMs)
{
    qCDebug(dragonMultimediaPlayer) << "seek(" << posMs << ")";
    setPosition(posMs);
}

void DragonPlayer::saveUndoPosition(qint64 posMs)
{
    qCDebug(dragonMultimediaPlayer) << "saveUndoPosition(" << posMs << ")";
    d->undoPosition = posMs;
}

void DragonPlayer::restoreUndoPosition()
{
    qCDebug(dragonMultimediaPlayer) << "restoreUndoPosition()";
    if (d->undoPosition > 0) {
        setPosition(d->undoPosition);
    }
}
