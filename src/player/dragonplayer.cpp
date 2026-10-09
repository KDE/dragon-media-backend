/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"

#include "dragonpositionestimator.h"
#include "sink/dragonaudiosinkfactory.h"
#include "stream/dragonstreamfactory.h"
#include <dragonaudiooutput.h>
#include <dragonfftframe.h>
#include <dragonplayer.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#include <QCoroTask>
#pragma GCC diagnostic pop
#include <KLocalizedString>
#include <QMetaObject>
#include <QTimer>
#include <dragonmediabackend_logging.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

DragonPlayerPrivate::DragonPlayerPrivate(DragonPlayer *player, DragonAudioOutput::Backend requestedBackend)
    : QObject(player)
    , q(player)
    , decodePipeline(player)
    , requestedBackend(requestedBackend)
    , aliveGuard(std::make_shared<AliveGuard>())
{
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent)
{
    switch (intent) {
    case DragonPlayer::PlaybackState::PlayingState:
        if (!audioOutput->sink()->isDeviceOpen() || !audioOutput->sink()->hasFormat(sampleRate, channels)) {
            audioOutput->sink()->open(sampleRate, channels);
        } else {
            audioOutput->sink()->resume();
        }
        if (!audioOutput->sink()->isDeviceOpen()) {
            reportDeviceOpenFailure();
            return;
        }
        audioOutput->sink()->setQueueReady(!audioOutput->sink()->isFlushPending());
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        break;

    case DragonPlayer::PlaybackState::PausedState:
        if (!audioOutput->sink()->isDeviceOpen() || !audioOutput->sink()->hasFormat(sampleRate, channels)) {
            audioOutput->sink()->open(sampleRate, channels);
        }
        if (!audioOutput->sink()->isDeviceOpen()) {
            reportDeviceOpenFailure();
            return;
        }
        audioOutput->sink()->pause();
        audioOutput->sink()->setQueueReady(!audioOutput->sink()->isFlushPending());
        setPlaybackState(DragonPlayer::PlaybackState::PausedState);
        break;

    case DragonPlayer::PlaybackState::StoppedState:
        break;
    }
}

void DragonPlayerPrivate::reportDeviceOpenFailure()
{
    // Generic contract check for every backend: a sink whose open() did not
    // result in an open device cannot play. Sinks may report specifics via
    // errorOccurred() during open(); supply a fallback otherwise. The player
    // stays stopped instead of silently pretending to play.
    qCWarning(dragonMediaBackendPlayer) << "audio device failed to open, refusing playback state transition";
    if (currentError == DragonPlayer::Error::NoError) {
        setError(DragonPlayer::Error::ResourceError, i18n("Failed to open the audio output device."));
    }
}

void DragonPlayerPrivate::onSinkTrackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration)
{
    qCDebug(dragonMediaBackendPlayer) << "onSinkTrackStarted trackId=" << trackId << "duration=" << duration.value_or(-1ms).count();

    PendingTrack pending;
    {
        std::scoped_lock lock(m_pendingTracksMutex);
        const auto it = m_pendingTracks.find(trackId);
        if (it == m_pendingTracks.end()) {
            qCDebug(dragonMediaBackendPlayer) << "onSinkTrackStarted: no pending track for id, ignoring (stale)";
            return;
        }
        pending = it.value();
        m_pendingTracks.erase(it);
    }

    applyPendingTrack(pending);
}

void DragonPlayerPrivate::applyPendingTrack(const PendingTrack &pending)
{
    currentSource = pending.url;
    aboutToFinishEmitted = false;
    qCDebug(dragonMediaBackendPlayer) << "applyPendingTrack: reset aboutToFinishEmitted for" << currentSource.toString();
    currentIsLocal = DragonStreamFactory::isLocalSource(currentSource);
    currentSeekable = pending.seekable;
    currentDuration = pending.duration;
    currentSampleRate = pending.sampleRate;
    currentChannels = pending.channels;

    audioOutput->sink()->setStreamName(QString());

    positionEstimator->trackChanged(currentDuration);

    if (nextSource == currentSource) {
        nextSource.clear();
        Q_EMIT q->nextSourceChanged();
    }

    Q_EMIT q->trackChanged();
    Q_EMIT q->sourceChanged();
    Q_EMIT q->seekableChanged(currentSeekable);

    if (currentDuration) {
        Q_EMIT q->durationChanged(*currentDuration);
    }
}

void DragonPlayerPrivate::onSinkFormatChangeRequired(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendPlayer) << "onSinkFormatChangeRequired sr=" << sampleRate << "ch=" << channels;

    auto *sink = audioOutput ? audioOutput->sink() : nullptr;
    if (!sink) {
        return;
    }
    sink->close();
    sink->open(sampleRate, channels);
    if (!sink->isDeviceOpen()) {
        reportDeviceOpenFailure();
        return;
    }
    sink->setQueueReady(!sink->isFlushPending());
}

bool DragonPlayerPrivate::onPipelineBoundary(const DragonDecodePipeline::BoundaryEvent &event)
{
    DragonAudioSink *sink = audioOutput ? audioOutput->sink() : nullptr;

    switch (event.kind) {
    case DragonDecodePipeline::BoundaryEvent::Kind::TrackStart: {
        if (!sink) {
            return true;
        }
        // The epoch the producer observed before its last write: the boundary
        // is stamped with it, so a stream superseded by a flush (seek or track
        // change racing the promotion) yields a boundary the sink discards
        // instead of a ghost boundary firing in the new stream.
        const std::uint64_t producerEpoch = m_producerStreamEpoch.load(std::memory_order_acquire);
        // A provisional end of stream (armed when the current track hit EOF)
        // is retracted as long as the sink has not played past it. If it has,
        // the audio really ended: the next source starts as a regular track
        // from the drained() handler instead.
        if (!sink->retractEndOfStream()) {
            qCDebug(dragonMediaBackendPlayer) << "boundary: sink already drained, refusing gapless handoff of" << event.url.toString();
            return false;
        }
        if (sink->streamEpoch() != producerEpoch) {
            qCDebug(dragonMediaBackendPlayer) << "boundary: stream superseded by a flush, refusing gapless handoff of" << event.url.toString();
            return false;
        }
        const auto trackId = m_trackSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        {
            std::scoped_lock lock(m_pendingTracksMutex);
            m_pendingTracks.insert(trackId, PendingTrack{event.url, event.sampleRate, event.channels, event.duration, event.seekable});
        }
        if (!sink->boundaryTrackStart(trackId, event.sampleRate, event.channels, event.duration, producerEpoch)) {
            std::scoped_lock lock(m_pendingTracksMutex);
            m_pendingTracks.remove(trackId);
            qCWarning(dragonMediaBackendPlayer) << "boundary ring full; the track switch of" << event.url.toString() << "will not be reported at play time";
        }
        return true;
    }
    case DragonDecodePipeline::BoundaryEvent::Kind::EndOfStream:
        if (sink && !sink->boundaryEndOfStream()) {
            qCWarning(dragonMediaBackendPlayer) << "boundary: could not arm end of stream (one is already armed or claimed)";
        }
        return true;
    }
    return false;
}

void DragonPlayerPrivate::onDecodeFinished(const QUrl &source, bool hadFatalError)
{
    qCDebug(dragonMediaBackendPlayer) << "onDecodeFinished called source=" << source.toString() << "currentSource=" << currentSource.toString()
                                      << "hadFatalError=" << hadFatalError;

    if (source != currentSource) {
        qCDebug(dragonMediaBackendPlayer) << "ignoring stale onDecodeFinished (source mismatch)";
        return;
    }

    if (hadFatalError) {
        if (audioOutput) {
            audioOutput->sink()->close();
            audioOutput->sink()->setQueueReady(false);
        }
        setStatus(DragonPlayer::MediaStatus::InvalidMedia);
        if (currentError != DragonPlayer::Error::NoError && currentErrorString.isEmpty()) {
            currentErrorString = i18n("Fatal error while decoding media");
            Q_EMIT q->errorChanged(currentError);
        }
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        qCDebug(dragonMediaBackendPlayer) << "onDecodeFinished state/status changes complete (fatal)";
        return;
    }

    if (prefinishMark > 0ms && !aboutToFinishEmitted && audioOutput && currentDuration) {
        const qint64 pipeBacklog = static_cast<qint64>(audioPipe.consumer().ready());
        const qint64 eventualSamples = audioOutput->sink()->totalSamplesWritten() + pipeBacklog;
        const std::chrono::milliseconds eventualPosition{(eventualSamples / currentChannels) * 1000 / currentSampleRate};
        const std::chrono::milliseconds remaining = *currentDuration - eventualPosition;
        if (remaining <= prefinishMark) {
            aboutToFinishEmitted = true;
            qCDebug(dragonMediaBackendPlayer) << "aboutToFinish emitted from onDecodeFinished"
                                              << "eventualPos=" << eventualPosition << "remaining=" << remaining;
            Q_EMIT q->aboutToFinish();
        }
    }

    qCDebug(dragonMediaBackendPlayer) << "onDecodeFinished media ended, deferring StoppedState until drain";
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

// the stream signals come from the network I/O system, unlike the rest of the states.
// must ensure they don't change the status until the song is loaded, this is just a state machine design choice
// also block them when they don't make sense, like NoMedia and InvalidMedia
bool DragonPlayerPrivate::streamStateTransitionsAllowed() const
{
    return currentStatus != DragonPlayer::MediaStatus::NoMedia && currentStatus != DragonPlayer::MediaStatus::LoadingMedia
        && currentStatus != DragonPlayer::MediaStatus::InvalidMedia;
}

void DragonPlayerPrivate::onStreamStalled()
{
    if (!streamStateTransitionsAllowed()) {
        return;
    }
    setStatus(DragonPlayer::MediaStatus::StalledMedia);
}

void DragonPlayerPrivate::onStreamBuffering()
{
    if (!streamStateTransitionsAllowed()) {
        return;
    }
    setStatus(DragonPlayer::MediaStatus::BufferingMedia);
}

void DragonPlayerPrivate::onStreamBuffered()
{
    if (!streamStateTransitionsAllowed()) {
        return;
    }
    setStatus(DragonPlayer::MediaStatus::BufferedMedia);
}

void DragonPlayerPrivate::onStreamSeekable(bool seekable)
{
    if (currentSeekable != seekable) {
        currentSeekable = seekable;
        Q_EMIT q->seekableChanged(seekable);
    }
}

void DragonPlayerPrivate::writeToQueues(std::span<const float> pcm, const std::stop_token &st)
{
    if (pcm.empty()) {
        return;
    }

    while (audioOutput && !audioOutput->sink()->isQueueReady() && !st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (st.stop_requested()) {
        return;
    }

    const uint64_t generation = contentGeneration.load(std::memory_order_acquire);
    DragonAudioSink *sink = audioOutput ? audioOutput->sink() : nullptr;
    const std::uint64_t streamEpoch = sink ? sink->streamEpoch() : 0;
    if (sink) {
        // The epoch the producer last observed; TrackStart boundaries are
        // stamped with it (see onPipelineBoundary()).
        m_producerStreamEpoch.store(streamEpoch, std::memory_order_release);
    }
    const size_t written = audioPipe.producer().write(pcm, st, [this, generation]() {
        return contentGeneration.load(std::memory_order_acquire) != generation;
    });

    if (written > 0 && sink) {
        sink->samplesProduced(streamEpoch, written);
    }
}

void DragonPlayerPrivate::invalidateQueuedContent()
{
    contentGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void DragonPlayerPrivate::setPlaybackState(DragonPlayer::PlaybackState state)
{
    qCDebug(dragonMediaBackendPlayer) << "setPlaybackState(" << state << ") current=" << currentPlaybackState;
    if (currentPlaybackState == state) {
        qCDebug(dragonMediaBackendPlayer) << "setPlaybackState no change, returning";
        return;
    }

    const auto oldState = currentPlaybackState;

    if (state == DragonPlayer::PlaybackState::PlayingState) {
        positionEstimator->start();
    } else {
        positionEstimator->freeze();
    }

    currentPlaybackState = state;

    Q_EMIT q->stateChanged(state, oldState);
}

void DragonPlayerPrivate::setStatus(DragonPlayer::MediaStatus status)
{
    qCDebug(dragonMediaBackendPlayer) << "setStatus(" << status << ") current=" << currentStatus;
    if (currentStatus == status) {
        qCDebug(dragonMediaBackendPlayer) << "setStatus no change, returning";
        return;
    }
    currentStatus = status;
    qCDebug(dragonMediaBackendPlayer) << "setStatus emitting statusChanged";
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
    qCDebug(dragonMediaBackendPlayer) << "stopPipeline() full teardown";

    decodePipeline.stop();

    if (audioOutput && audioOutput->sink()) {
        audioOutput->sink()->close();
        audioOutput->sink()->reset();
    }

    qCDebug(dragonMediaBackendPlayer) << "stopPipeline() teardown complete";
}

DragonAudioSink *DragonPlayerPrivate::audioSink() const
{
    return audioOutput ? audioOutput->sink() : nullptr;
}

void DragonPlayerPrivate::init()
{
    audioOutput = new DragonAudioOutput(requestedBackend, q);
    if (audioOutput->sink()) {
        audioOutput->sink()->setAudioPipe(&audioPipe);
        audioOutput->sink()->setFftPipe(nullptr);
    }

    positionEstimator = new DragonPositionEstimator(this);
    positionEstimator->setDevicePositionCallback([this]() -> std::optional<std::chrono::milliseconds> {
        if (audioOutput && audioOutput->sink() && audioOutput->sink()->isDeviceOpen()) {
            return audioOutput->sink()->position();
        }
        return std::nullopt;
    });
    connect(positionEstimator, &DragonPositionEstimator::positionChanged, q, &DragonPlayer::positionChanged);
    connect(positionEstimator, &DragonPositionEstimator::positionChanged, this, [this](std::chrono::milliseconds pos) {
        if (prefinishMark <= 0ms) {
            return;
        }
        if (currentDuration && *currentDuration > 0ms && !aboutToFinishEmitted) {
            const std::chrono::milliseconds remaining = *currentDuration - pos;
            if (remaining <= prefinishMark && remaining > 0ms) {
                aboutToFinishEmitted = true;
                qCDebug(dragonMediaBackendPlayer) << "aboutToFinish emitted remaining=" << remaining << ", prefinishMark=" << prefinishMark;
                Q_EMIT q->aboutToFinish();
            }
        }
    });

    connect(audioOutput->sink(), &DragonAudioSink::errorOccurred, this, [this](const QString &message) {
        setError(DragonPlayer::Error::ResourceError, message);
    });

    connect(audioOutput->sink(), &DragonAudioSink::drained, this, [this]() {
        if (decodePipeline.isActive()) {
            qCDebug(dragonMediaBackendPlayer) << "drained ignored, decode session still active (stale drain from previous track)";
            return;
        }
        if (currentDuration && *currentDuration > 0ms && currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
            positionEstimator->snapToDuration();
        }
        setStatus(DragonPlayer::MediaStatus::EndOfMedia);
        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);

        if (!nextSource.isEmpty()) {
            const QUrl next = nextSource;
            QMetaObject::invokeMethod(
                this,
                [this, next]() {
                    if (nextSource == next) {
                        qCDebug(dragonMediaBackendPlayer) << "drained with pending nextSource, starting" << next.toString() << "as a regular track";
                        q->setSource(next);
                        q->play();
                    }
                },
                Qt::QueuedConnection);
        }
    });

    connect(audioOutput->sink(), &DragonAudioSink::trackStarted, this, &DragonPlayerPrivate::onSinkTrackStarted, Qt::QueuedConnection);

    connect(audioOutput->sink(), &DragonAudioSink::formatChangeRequired, this, &DragonPlayerPrivate::onSinkFormatChangeRequired, Qt::QueuedConnection);

    decodePipeline.setSamplesCallback([this](auto samples, const std::stop_token &st) {
        writeToQueues(samples, st);
    });

    decodePipeline.setBoundaryCallback([this](const DragonDecodePipeline::BoundaryEvent &event) {
        return onPipelineBoundary(event);
    });

    connect(&decodePipeline, &DragonDecodePipeline::sessionError, this, &DragonPlayerPrivate::onDecodeError, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::sessionFinished, this, &DragonPlayerPrivate::onDecodeFinished, Qt::QueuedConnection);

    connect(&decodePipeline, &DragonDecodePipeline::streamStalled, this, &DragonPlayerPrivate::onStreamStalled, Qt::QueuedConnection);
    connect(&decodePipeline, &DragonDecodePipeline::streamBuffering, this, &DragonPlayerPrivate::onStreamBuffering, Qt::QueuedConnection);
    connect(&decodePipeline, &DragonDecodePipeline::streamBuffered, this, &DragonPlayerPrivate::onStreamBuffered, Qt::QueuedConnection);
    connect(&decodePipeline, &DragonDecodePipeline::streamSeekableChanged, this, &DragonPlayerPrivate::onStreamSeekable, Qt::QueuedConnection);

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
}

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DragonFftFrame>();

    d = std::make_unique<DragonPlayerPrivate>(this, DragonAudioOutput::Backend::Auto);
    d->init();

    connect(this, &DragonPlayer::stateChanged, this, [](PlaybackState newState, PlaybackState oldState) {
        qCDebug(dragonMediaBackendPlayer) << "playbackState changed from" << oldState << "to" << newState;
    });
    connect(this, &DragonPlayer::statusChanged, this, [](MediaStatus status) {
        qCDebug(dragonMediaBackendPlayer) << "mediaStatus changed to" << status;
    });
}

DragonPlayer::DragonPlayer(DragonAudioOutput::Backend requestedBackend, QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DragonFftFrame>();

    d = std::make_unique<DragonPlayerPrivate>(this, requestedBackend);
    d->init();

    connect(this, &DragonPlayer::stateChanged, this, [](PlaybackState newState, PlaybackState oldState) {
        qCDebug(dragonMediaBackendPlayer) << "playbackState changed from" << oldState << "to" << newState;
    });
    connect(this, &DragonPlayer::statusChanged, this, [](MediaStatus status) {
        qCDebug(dragonMediaBackendPlayer) << "mediaStatus changed to" << status;
    });
}

DragonPlayer::~DragonPlayer()
{
    if (d && d->aliveGuard) {
        d->aliveGuard->alive = false;
    }
    d->stopPipeline();
}

DragonAudioOutput *DragonPlayer::audioOutput() const
{
    return d->audioOutput;
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
std::optional<std::chrono::milliseconds> DragonPlayer::duration() const
{
    return d->currentDuration;
}
std::chrono::milliseconds DragonPlayer::position() const
{
    return d->positionEstimator->position();
}
bool DragonPlayer::seekable() const
{
    return d->currentSeekable;
}
qreal DragonPlayer::bufferProgress() const
{
    return d->currentBufferProgress;
}

QCoro::Task<void> DragonPlayerPrivate::startLoad(QUrl source, uint64_t generation)
{
    auto aliveGuard = this->aliveGuard;

    auto result = co_await decodePipeline.initializeSession(source);

    if (!aliveGuard || !aliveGuard->alive) {
        co_return;
    }

    if (generation != loadGeneration) {
        qCDebug(dragonMediaBackendPlayer) << "startLoad superseded (generation" << generation << "!= current" << loadGeneration << "), discarding";
        co_return;
    }

    if (!result.success) {
        if (result.cancelled) {
            qCDebug(dragonMediaBackendPlayer) << "startLoad cancelled, returning without state change";
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
    currentDuration = result.duration;
    positionEstimator->setDuration(currentDuration);

    if (currentError != DragonPlayer::Error::NoError || !currentErrorString.isEmpty()) {
        currentError = DragonPlayer::Error::NoError;
        currentErrorString.clear();
        Q_EMIT q->errorChanged(DragonPlayer::Error::NoError);
    }

    if (currentDuration) {
        Q_EMIT q->durationChanged(*currentDuration);
    }

    if (!currentIsLocal && decodePipeline.streamIsSeekable()) {
        currentSeekable = true;
        Q_EMIT q->seekableChanged(true);
    }

    setStatus(DragonPlayer::MediaStatus::LoadedMedia);

    applyRequestedState(result.sampleRate, result.channels, requestedPlaybackState);

    co_return;
}

void DragonPlayer::setSource(const QUrl &source)
{
    qCDebug(dragonMediaBackendPlayer) << "setSource(" << source.toString() << ")";

    bool playRequestedReload = d->playRequestedReload;
    d->playRequestedReload = false;

    if (!playRequestedReload && d->currentSource == source && source.isValid()) {
        const bool hasLoadedMedia = d->currentStatus == MediaStatus::LoadedMedia || d->currentStatus == MediaStatus::BufferedMedia
            || d->currentStatus == MediaStatus::StalledMedia || d->currentStatus == MediaStatus::EndOfMedia;

        const bool isSettled = d->currentPlaybackState == PlaybackState::StoppedState && hasLoadedMedia;

        if (!isSettled) {
            qCDebug(dragonMediaBackendPlayer) << "setSource(sameUrl) early return";
            if (d->currentStatus == MediaStatus::LoadingMedia) {
                d->requestedPlaybackState = PlaybackState::StoppedState;
                return; // Let the existing initialization finish
            }

            d->requestedPlaybackState = PlaybackState::StoppedState;
            stop();
            return;
        }

        qCDebug(dragonMediaBackendPlayer) << "setSource(sameUrl) while stopped with loaded media: reloading";
    }

    if (d->audioOutput) {
        d->invalidateQueuedContent();
        d->audioOutput->sink()->setQueueReady(false);
        d->audioOutput->sink()->setPositionOffset(0ms, DragonAudioSink::PositionResetMode::NormalTrackChange);
        if (d->currentSource != source) {
            d->audioOutput->sink()->setStreamName(QString());
        }
    }

    {
        std::scoped_lock lock(d->m_pendingTracksMutex);
        d->m_pendingTracks.clear();
    }

    d->decodePipeline.stopSession();

    const bool wasPlayingOrPaused = d->currentPlaybackState == PlaybackState::PlayingState || d->currentPlaybackState == PlaybackState::PausedState;

    d->currentSource = source;
    d->positionEstimator->resetPosition(0ms);
    d->currentDuration = {};
    d->positionEstimator->setDuration({});
    d->aboutToFinishEmitted = false;
    qCDebug(dragonMediaBackendPlayer) << "setSource: reset aboutToFinishEmitted for" << d->currentSource.toString();
    d->decodePipeline.clearNextSource();
    d->nextSource.clear();
    d->currentSampleRate = 0;
    d->currentChannels = 0;

    if (!playRequestedReload) {
        d->requestedPlaybackState = PlaybackState::StoppedState;
        if (wasPlayingOrPaused) {
            d->setPlaybackState(PlaybackState::StoppedState);
        }

        if (d->currentSource.isEmpty()) {
            ++d->loadGeneration;
            if (d->audioOutput) {
                d->audioOutput->sink()->close();
                d->audioOutput->sink()->reset();
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
        qCDebug(dragonMediaBackendPlayer) << "setSource internal reload preserving Playing intent";
        d->setStatus(MediaStatus::LoadingMedia);
    }

    const bool isLocal = DragonStreamFactory::isLocalSource(d->currentSource);
    const bool isHttp = d->currentSource.scheme() == QStringLiteral("http") || d->currentSource.scheme() == QStringLiteral("https");

    d->currentIsLocal = isLocal;
    d->currentSeekable = !isHttp;
    Q_EMIT seekableChanged(d->currentSeekable);

    ++d->loadGeneration;

    const uint64_t generation = d->loadGeneration;
    const QUrl sourceToLoad = d->currentSource;
    auto *priv = d.get();
    QMetaObject::invokeMethod(
        priv,
        [priv, sourceToLoad, generation]() {
            priv->startLoad(sourceToLoad, generation);
        },
        Qt::QueuedConnection);
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    qCDebug(dragonMediaBackendPlayer) << "setNextSource(" << nextSource.toString() << ")";
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline.setNextSource(nextSource);
}

void DragonPlayer::setStreamName(const QString &name)
{
    qCDebug(dragonMediaBackendPlayer) << "setStreamName(" << name << ")";

    if (auto *sink = d->audioSink()) {
        sink->setStreamName(name);
    }
}

void DragonPlayerPrivate::resolvePendingTrackForSeek()
{
    // A promoted track is queued but not yet audible: the decoder is already
    // on it while the player still reports the old track. The seek's flush
    // drops the pending TrackStart boundary (trackStarted would never fire),
    // so resolve the switch here: the player reports the track the decoder
    // is actually on, and the seek applies within it.
    std::optional<PendingTrack> resolved;
    {
        std::scoped_lock lock(m_pendingTracksMutex);
        if (m_pendingTracks.isEmpty()) {
            return;
        }
        // Entries are inserted in promotion order (increasing trackId); the
        // highest one is the track the decoder is on. The audible tail is
        // flushed by the seek anyway, so intermediate switches are skipped.
        std::uint32_t lastId = 0;
        for (auto it = m_pendingTracks.cbegin(); it != m_pendingTracks.cend(); ++it) {
            lastId = std::max(lastId, it.key());
        }
        resolved = m_pendingTracks.value(lastId);
        m_pendingTracks.clear();
    }
    qCDebug(dragonMediaBackendPlayer) << "resolving pending track switch at seek time:" << resolved->url.toString();
    applyPendingTrack(*resolved);
}

void DragonPlayer::setPosition(std::chrono::milliseconds position)
{
    qCDebug(dragonMediaBackendPlayer) << "setPosition(" << position << ")";
    std::chrono::milliseconds upperBound = d->currentDuration ? std::max(*d->currentDuration, 0ms) : 0ms;
    position = std::clamp(position, 0ms, upperBound);

    d->invalidateQueuedContent();
    // requestSeek() waits out an in-flight gapless promotion, so by the time
    // it returns the promoted track's boundary and pending entry (if any)
    // exist and the seek targets the session that ends up current.
    d->decodePipeline.requestSeek(position);

    d->resolvePendingTrackForSeek();

    // The pending switch may have changed the reported track (and with it
    // the duration the position must be clamped against).
    upperBound = d->currentDuration ? std::max(*d->currentDuration, 0ms) : 0ms;
    position = std::clamp(position, 0ms, upperBound);

    if (d->audioOutput) {
        // The promoted track may need a different device format than the
        // track it replaces; without a reopen the new samples would play at
        // the old rate/channel count. Mirrors onSinkFormatChangeRequired.
        if (!d->audioOutput->sink()->hasFormat(d->currentSampleRate, d->currentChannels) && d->currentSampleRate > 0) {
            d->audioOutput->sink()->close();
            d->audioOutput->sink()->open(d->currentSampleRate, d->currentChannels);
            if (!d->audioOutput->sink()->isDeviceOpen()) {
                d->reportDeviceOpenFailure();
                return;
            }
        }
        d->audioOutput->sink()->setPositionOffset(position, DragonAudioSink::PositionResetMode::Seek);
        d->audioOutput->sink()->clearStream();
    }
    d->positionEstimator->seek(position);

    if (d->prefinishMark > 0ms && d->currentDuration && *d->currentDuration > 0ms) {
        const std::chrono::milliseconds remaining = *d->currentDuration - position;
        if (remaining > d->prefinishMark) {
            d->aboutToFinishEmitted = false;
        }
    }

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        d->currentStatus = MediaStatus::LoadedMedia;
        Q_EMIT statusChanged(MediaStatus::LoadedMedia);
    }
}

std::chrono::milliseconds DragonPlayer::prefinishMark() const
{
    return d->prefinishMark;
}

void DragonPlayer::setPrefinishMark(std::chrono::milliseconds msec)
{
    qCDebug(dragonMediaBackendPlayer) << "setPrefinishMark(" << msec << ")";
    if (d->prefinishMark == msec) {
        return;
    }
    d->prefinishMark = msec;
    Q_EMIT prefinishMarkChanged(msec);

    if (d->prefinishMark > 0ms && d->currentDuration && *d->currentDuration > 0ms) {
        const std::chrono::milliseconds remaining = *d->currentDuration - position();
        if (remaining > d->prefinishMark) {
            d->aboutToFinishEmitted = false;
        }
    }
}

void DragonPlayer::play()
{
    qCDebug(dragonMediaBackendPlayer) << "play()";
    if (d->currentSource.isEmpty()) {
        return;
    }

    d->requestedPlaybackState = PlaybackState::PlayingState;

    if (d->currentPlaybackState == PlaybackState::PlayingState) {
        return;
    }

    if (d->currentPlaybackState == PlaybackState::PausedState) {
        if (d->decodePipeline.isActive() && d->audioOutput && d->audioOutput->sink()->isDeviceOpen()) {
            d->audioOutput->sink()->resume();
            d->setPlaybackState(PlaybackState::PlayingState);
            return;
        }
        qCDebug(dragonMediaBackendPlayer) << "play() paused but pipeline/sink not alive, restarting";
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonMediaBackendPlayer) << "play() status is LoadingMedia, intent captured";
        return;
    }

    if (d->currentStatus == MediaStatus::EndOfMedia) {
        qCDebug(dragonMediaBackendPlayer) << "play() status is EndOfMedia, reloading source";
        d->playRequestedReload = true;
        d->requestedPlaybackState = PlaybackState::PlayingState;
        setSource(d->currentSource);
        return;
    }

    if (!d->decodePipeline.isActive() && !d->currentSource.isEmpty()) {
        qCDebug(dragonMediaBackendPlayer) << "play() decoder not active, restarting decode pipeline";
        d->playRequestedReload = true;
        d->requestedPlaybackState = PlaybackState::PlayingState;
        setSource(d->currentSource);
        return;
    }

    qCDebug(dragonMediaBackendPlayer) << "play() status is " << d->currentStatus << ", starting audio synchronously";

    if (d->audioOutput && !d->audioOutput->sink()->isDeviceOpen() && d->currentSampleRate > 0) {
        d->audioOutput->sink()->open(d->currentSampleRate, d->currentChannels);
    }
    if (d->audioOutput && !d->audioOutput->sink()->isDeviceOpen()) {
        d->reportDeviceOpenFailure();
        return;
    }
    if (d->audioOutput) {
        d->audioOutput->sink()->setQueueReady(!d->audioOutput->sink()->isFlushPending());
    }
    d->setPlaybackState(PlaybackState::PlayingState);
}

void DragonPlayer::pause()
{
    qCDebug(dragonMediaBackendPlayer) << "pause()";

    if (d->currentStatus == MediaStatus::NoMedia || d->currentStatus == MediaStatus::InvalidMedia) {
        return;
    }

    if (d->currentPlaybackState == PlaybackState::PausedState) {
        d->requestedPlaybackState = PlaybackState::PausedState;
        return;
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonMediaBackendPlayer) << "pause() status is LoadingMedia, intent captured";
        d->requestedPlaybackState = PlaybackState::PausedState;
        return;
    }

    if (d->currentPlaybackState == PlaybackState::StoppedState) {
        qCDebug(dragonMediaBackendPlayer) << "pause() while stopped, ignored";
        return;
    }

    d->requestedPlaybackState = PlaybackState::PausedState;

    if (d->audioOutput) {
        d->audioOutput->sink()->pause();
    }
    d->setPlaybackState(PlaybackState::PausedState);
}

void DragonPlayer::stop()
{
    qCDebug(dragonMediaBackendPlayer) << "stop()";

    d->requestedPlaybackState = PlaybackState::StoppedState;

    if (!d->nextSource.isEmpty()) {
        d->nextSource.clear();
        qCDebug(dragonMediaBackendPlayer) << "stop(): disarming next source";
        Q_EMIT nextSourceChanged();
        d->decodePipeline.setNextSource(QUrl());
    }

    {
        std::scoped_lock lock(d->m_pendingTracksMutex);
        d->m_pendingTracks.clear();
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        // stop()s playback but must not interrupt an in-flight load nor change media status
        qCDebug(dragonMediaBackendPlayer) << "stop() during LoadingMedia, letting in-flight load continue";
        if (d->audioOutput) {
            d->audioOutput->sink()->close();
            d->audioOutput->sink()->reset();
        }
        d->setPlaybackState(PlaybackState::StoppedState);
        if (d->positionEstimator->position() != 0ms) {
            d->positionEstimator->seek(0ms);
        }
        return;
    }

    d->decodePipeline.stopSession();

    if (d->audioOutput) {
        d->audioOutput->sink()->close();
        d->audioOutput->sink()->reset();
    }

    d->setPlaybackState(PlaybackState::StoppedState);

    if (d->positionEstimator->position() != 0ms) {
        d->positionEstimator->seek(0ms);
    }

    if (d->currentStatus != MediaStatus::NoMedia && d->currentStatus != MediaStatus::InvalidMedia) {
        d->setStatus(MediaStatus::LoadedMedia);
    }
}
