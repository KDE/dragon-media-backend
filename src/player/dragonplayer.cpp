/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"

#include "positionestimator.h"
#include "sink/playbackpipelinefactory.h"
#include "stream/streamfactory.h"
#include <dragonaudiooutput.h>
#include <dragonfftframe.h>
#include <dragonplayer.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroTask>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
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
    , requestedBackend(requestedBackend)
    , aliveGuard(std::make_shared<AliveGuard>())
{
}

void DragonPlayerPrivate::applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent)
{
    auto *sink = audioOutput ? audioOutput->playbackPipeline() : nullptr;

    switch (intent) {
    case DragonPlayer::PlaybackState::PlayingState:
        if (!sink->isDeviceOpen() || !sink->hasFormat(sampleRate, channels)) {
            sink->open(sampleRate, channels);
        } else {
            sink->resume();
        }
        if (!sink->isDeviceOpen() && sink->isCrossfading()) {
            // The incoming stream could not be created while the outgoing
            // fade still holds the only server stream: abort the fade under
            // player control and retry the primary stream solo.
            fadingSlot().decodePipeline.stopSession();
            sink->abortFadingVoice();
            sink->open(sampleRate, channels);
        }
        if (!sink->isDeviceOpen()) {
            reportDeviceOpenFailure();
            return;
        }
        sink->reopenIfIdle();
        if (currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
            positionEstimator->start();
        } else {
            setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        }
        break;

    case DragonPlayer::PlaybackState::PausedState:
        if (!sink->isDeviceOpen() || !sink->hasFormat(sampleRate, channels)) {
            sink->open(sampleRate, channels);
        }
        if (!sink->isDeviceOpen() && sink->isCrossfading()) {
            fadingSlot().decodePipeline.stopSession();
            sink->abortFadingVoice();
            sink->open(sampleRate, channels);
        }
        if (!sink->isDeviceOpen()) {
            reportDeviceOpenFailure();
            return;
        }
        sink->reopenIfIdle();
        sink->pause();
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
    // stops cleanly (including any live fading voice) instead of pretending to play.
    qCWarning(dragonMediaBackendPlayer) << "audio device failed to open, refusing playback state transition";
    for (auto &slot : m_slots) {
        invalidateSlotContent(slot);
        slot.decodePipeline.stopSession();
    }
    teardownPipeline();
    for (auto &slot : m_slots) {
        slot.audioPipe.consumer().drain();
        slot.audioPipe.setProducerDone(false);
    }
    if (currentError == DragonPlayer::Error::NoError) {
        setError(DragonPlayer::Error::ResourceError, i18n("Failed to open the audio output device."));
    }
    setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
}

void DragonPlayerPrivate::onPlaybackTrackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration)
{
    qCDebug(dragonMediaBackendPlayer) << "onPlaybackTrackStarted trackId=" << trackId << "duration=" << duration.value_or(-1ms).count();

    PendingTrack pending;
    {
        std::scoped_lock lock(m_pendingTracksMutex);
        const auto it = m_pendingTracks.constFind(trackId);
        if (it == m_pendingTracks.constEnd()) {
            qCDebug(dragonMediaBackendPlayer) << "onPlaybackTrackStarted: no pending track for id, ignoring (stale)";
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
    currentSourceOptions = DragonSourceOptions{};
    aboutToFinishEmitted = false;
    qCDebug(dragonMediaBackendPlayer) << "applyPendingTrack: reset aboutToFinishEmitted for" << currentSource.toString();
    currentIsLocal = StreamFactory::isLocalSource(currentSource);
    currentSeekable = pending.seekable;
    currentDuration = pending.duration;
    currentSampleRate = pending.sampleRate;
    currentChannels = pending.channels;

    const QString priorStreamName = audioOutput->playbackPipeline()->streamName();
    audioOutput->playbackPipeline()->setStreamName(QString());
    if (!priorStreamName.isEmpty()) {
        Q_EMIT q->streamNameChanged(QString());
    }

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

void DragonPlayerPrivate::onPlaybackFormatChangeRequired(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendPlayer) << "onPlaybackFormatChangeRequired sr=" << sampleRate << "ch=" << channels;

    if (currentPlaybackState == DragonPlayer::PlaybackState::StoppedState || currentStatus == DragonPlayer::MediaStatus::NoMedia
        || currentStatus == DragonPlayer::MediaStatus::LoadingMedia || currentStatus == DragonPlayer::MediaStatus::InvalidMedia) {
        qCDebug(dragonMediaBackendPlayer) << "onPlaybackFormatChangeRequired ignored in state" << currentPlaybackState << "status" << currentStatus;
        return;
    }

    auto *sink = audioOutput ? audioOutput->playbackPipeline() : nullptr;
    if (!sink) {
        return;
    }
    if (!sink->close()) {
        setError(DragonPlayer::Error::ResourceError, QStringLiteral("Audio callback did not quiesce on stream close"));
        reportDeviceOpenFailure();
        return;
    }
    applyRequestedState(sampleRate, channels, currentPlaybackState);
}

bool DragonPlayerPrivate::onPipelineBoundary(PipelineSlot &slot, const DecodePipeline::BoundaryEvent &event)
{
    PlaybackPipeline *sink = audioOutput ? audioOutput->playbackPipeline() : nullptr;

    switch (event.kind) {
    case DecodePipeline::BoundaryEvent::Kind::TrackStart: {
        if (!sink) {
            return true;
        }
        // The epoch the producer observed before its last write: the boundary
        // is stamped with it, so a stream superseded by a flush (seek or track
        // change racing the promotion) yields a boundary the sink discards
        // instead of a ghost boundary firing in the new stream.
        const std::uint64_t producerEpoch = slot.producerStreamEpoch.load(std::memory_order_acquire);
        // A provisional end of stream (armed when the current track hit EOF)
        // is retracted as long as the sink has not played past it. If it has,
        // the audio really ended: the next source starts as a regular track
        // from the drained() handler instead.
        if (!sink->retractEndOfStreamForPipe(&slot.audioPipe)) {
            qCDebug(dragonMediaBackendPlayer) << "boundary: sink already drained or demoted, refusing gapless handoff of" << event.url.toString();
            return false;
        }
        if (sink->streamEpochForPipe(&slot.audioPipe) != producerEpoch) {
            qCDebug(dragonMediaBackendPlayer) << "boundary: stream superseded by a flush, refusing gapless handoff of" << event.url.toString();
            return false;
        }
        const auto trackId = m_trackSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        {
            std::scoped_lock lock(m_pendingTracksMutex);
            m_pendingTracks.insert(trackId, PendingTrack{event.url, event.sampleRate, event.channels, event.duration, event.seekable});
        }
        if (!sink->boundaryTrackStartForPipe(&slot.audioPipe, trackId, event.sampleRate, event.channels, event.duration, producerEpoch)) {
            std::scoped_lock lock(m_pendingTracksMutex);
            m_pendingTracks.remove(trackId);
            qCWarning(dragonMediaBackendPlayer) << "boundary ring full; the track switch of" << event.url.toString() << "will not be reported at play time";
            return false;
        }
        return true;
    }
    case DecodePipeline::BoundaryEvent::Kind::EndOfStream: {
        const std::uint64_t producerEpoch = slot.producerStreamEpoch.load(std::memory_order_acquire);
        if (sink && !sink->boundaryEndOfStreamForPipe(&slot.audioPipe, producerEpoch)) {
            qCWarning(dragonMediaBackendPlayer) << "boundary: could not arm end of stream (one is already armed or claimed)";
        }
        return true;
    }
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
        for (auto &slot : m_slots) {
            invalidateSlotContent(slot);
        }
        fadingSlot().decodePipeline.stopSession();
        teardownPipeline();
        for (auto &slot : m_slots) {
            slot.audioPipe.consumer().drain();
            slot.audioPipe.setProducerDone(false);
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
        const qint64 pipeBacklog = static_cast<qint64>(audioPipe().consumer().ready());
        const qint64 eventualSamples = audioOutput->playbackPipeline()->totalSamplesWritten() + pipeBacklog;
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

void DragonPlayerPrivate::writeToSlotQueue(PipelineSlot &slot, std::span<const float> pcm, const std::stop_token &st)
{
    PlaybackPipeline *sink = audioOutput ? audioOutput->playbackPipeline() : nullptr;
    const uint64_t token = slot.contentEpoch.load(std::memory_order_acquire);

    if (isPrimarySlot(slot) && slot.contentEpoch.load(std::memory_order_acquire) == token) {
        while (sink && !sink->isQueueReadyForPipe(&slot.audioPipe) && !st.stop_requested() && slot.contentEpoch.load(std::memory_order_acquire) == token) {
            std::this_thread::sleep_for(500us);
        }
        if (st.stop_requested()) {
            return;
        }

        if (isPrimarySlot(slot) && slot.contentEpoch.load(std::memory_order_acquire) == token) {
            // Refresh producerStreamEpoch here (after clearing the sink producer gate)
            // even for empty sync chunks so 0-chunk/EOF seeks stamp the post-seek epoch.
            const std::uint64_t streamEpoch = sink ? sink->streamEpochForPipe(&slot.audioPipe) : slot.streamEpoch.load(std::memory_order_acquire);
            if (streamEpoch != 0) {
                slot.producerStreamEpoch.store(streamEpoch, std::memory_order_release);
            }
            if (pcm.empty()) {
                return;
            }
            const size_t written = slot.audioPipe.producer().write(pcm, st, [&slot, token]() {
                return slot.contentEpoch.load(std::memory_order_acquire) != token;
            });

            if (written > 0 && sink && streamEpoch != 0) {
                sink->samplesProducedForPipe(&slot.audioPipe, streamEpoch, written);
            }
            return;
        }
    }

    if (pcm.empty()) {
        return;
    }

    // Retiring (fade-out) slot: write straight into its pipe without touching
    // the sink's producer state, which belongs exclusively to the primary slot.
    const size_t written = slot.audioPipe.producer().write(pcm, st, [&slot, &st, token]() {
        return st.stop_requested() || slot.contentEpoch.load(std::memory_order_acquire) != token;
    });
    std::ignore = written;
}

void DragonPlayerPrivate::invalidateQueuedContent()
{
    primarySlot().contentEpoch.fetch_add(1, std::memory_order_acq_rel);
}

void DragonPlayerPrivate::teardownPipeline()
{
    auto *sink = playbackPipeline();
    if (!sink) {
        return;
    }
    if (!sink->close()) {
        qCWarning(dragonMediaBackendPlayer) << "teardownPipeline: audio callback did not quiesce on stream close";
    }
    sink->reset();
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
        if (currentStatus != DragonPlayer::MediaStatus::LoadingMedia && playbackPipeline() && playbackPipeline()->isDeviceOpen()) {
            positionEstimator->start();
        }
    } else {
        positionEstimator->freeze();
    }

    currentPlaybackState = state;

    Q_EMIT q->playbackStateChanged(state, oldState);
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    Q_EMIT q->stateChanged(state, oldState);
    QT_WARNING_POP
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
    if (currentStatus != DragonPlayer::MediaStatus::InvalidMedia) {
        currentStatus = DragonPlayer::MediaStatus::InvalidMedia;
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::InvalidMedia);
    }
    currentError = error;
    Q_EMIT q->errorChanged(error);
}

void DragonPlayerPrivate::stopPipeline()
{
    qCDebug(dragonMediaBackendPlayer) << "stopPipeline() full teardown";

    for (auto &slot : m_slots) {
        slot.decodePipeline.stop();
    }

    if (audioOutput && audioOutput->playbackPipeline()) {
        if (!audioOutput->playbackPipeline()->close()) {
            qCWarning(dragonMediaBackendPlayer) << "stopPipeline: audio callback did not quiesce on stream close";
        }
        audioOutput->playbackPipeline()->reset();
    }

    qCDebug(dragonMediaBackendPlayer) << "stopPipeline() teardown complete";
}

PlaybackPipeline *DragonPlayerPrivate::playbackPipeline() const
{
    return audioOutput ? audioOutput->playbackPipeline() : nullptr;
}

void DragonPlayerPrivate::init()
{
    audioOutput = new DragonAudioOutput(requestedBackend, q);
    audioOutput->playbackPipeline()->setAudioPipe(&primarySlot().audioPipe);
    const auto initialEpoch = audioOutput->playbackPipeline()->streamEpoch();
    primarySlot().streamEpoch.store(initialEpoch, std::memory_order_release);
    primarySlot().producerStreamEpoch.store(initialEpoch, std::memory_order_release);
    audioOutput->playbackPipeline()->setFftPipe(nullptr);

    // Cleanup when a fading voice ends on its own (fade completed, early EOF,
    // or a fading-stream error): stop and drain the matching fading slot by
    // pipe identity so its decoder cannot remain parked on a full pipe.
    audioOutput->playbackPipeline()->setFadingVoiceEndedCallback([this](Pipe<float> *pipe, const QString &error) {
        for (auto &slot : m_slots) {
            if (&slot.audioPipe != pipe) {
                continue;
            }
            invalidateSlotContent(slot);
            slot.decodePipeline.stopSession();
            slot.audioPipe.consumer().drain();
            slot.audioPipe.setProducerDone(false);
            break;
        }
        if (!error.isEmpty()) {
            qCWarning(dragonMediaBackendPlayer) << "fading voice ended with a stream error:" << error;
        }
    });

    positionEstimator = new PositionEstimator(this);
    positionEstimator->setDevicePositionCallback([this]() -> std::optional<std::chrono::milliseconds> {
        if (audioOutput->playbackPipeline()->isDeviceOpen()) {
            return audioOutput->playbackPipeline()->position();
        }
        return std::nullopt;
    });
    connect(positionEstimator, &PositionEstimator::positionChanged, q, &DragonPlayer::positionChanged);
    connect(positionEstimator, &PositionEstimator::positionChanged, this, [this](std::chrono::milliseconds pos) {
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

    connect(audioOutput->playbackPipeline(), &PlaybackPipeline::errorOccurred, this, [this](const QString &message) {
        setError(DragonPlayer::Error::ResourceError, message);
    });

    connect(audioOutput->playbackPipeline(), &PlaybackPipeline::drained, this, [this]() {
        if (decodePipeline().isActive()) {
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
        } else if (audioOutput->playbackPipeline()->isDeviceOpen()) {
            // we're at the end of track, clean up the state
            if (!audioOutput->playbackPipeline()->close()) {
                qCWarning(dragonMediaBackendPlayer) << "drained: audio callback did not quiesce on stream close";
            }
            audioOutput->playbackPipeline()->reset();
        }
    });

    connect(audioOutput->playbackPipeline(), &PlaybackPipeline::trackStarted, this, &DragonPlayerPrivate::onPlaybackTrackStarted, Qt::QueuedConnection);

    connect(audioOutput->playbackPipeline(),
            &PlaybackPipeline::formatChangeRequired,
            this,
            &DragonPlayerPrivate::onPlaybackFormatChangeRequired,
            Qt::QueuedConnection);

    for (auto &slot : m_slots) {
        slot.decodePipeline.setSamplesCallback([this, &slot](std::span<const float> samples, const std::stop_token &st) {
            writeToSlotQueue(slot, samples, st);
        });

        slot.decodePipeline.setBoundaryCallback([this, &slot](const DecodePipeline::BoundaryEvent &event) {
            if (event.kind == DecodePipeline::BoundaryEvent::Kind::EndOfStream) {
                slot.audioPipe.setProducerDone(true);
            }
            if (!isPrimarySlot(slot)) {
                // Only the primary slot's boundaries may affect player and
                // sink state; reject any racing gapless TrackStart on a fading slot.
                return event.kind != DecodePipeline::BoundaryEvent::Kind::TrackStart;
            }
            return onPipelineBoundary(slot, event);
        });

        connect(
            &slot.decodePipeline,
            &DecodePipeline::sessionError,
            this,
            [this, &slot](const QString &message) {
                if (isPrimarySlot(slot)) {
                    onDecodeError(message);
                }
            },
            Qt::QueuedConnection);

        connect(
            &slot.decodePipeline,
            &DecodePipeline::sessionFinished,
            this,
            [this, &slot](const QUrl &source, bool hadFatalError) {
                if (!isPrimarySlot(slot)) {
                    if (hadFatalError) {
                        slot.audioPipe.setProducerDone(true);
                    }
                    return;
                }
                onDecodeFinished(source, hadFatalError);
            },
            Qt::QueuedConnection);

        connect(
            &slot.decodePipeline,
            &DecodePipeline::streamStalled,
            this,
            [this, &slot]() {
                if (isPrimarySlot(slot)) {
                    onStreamStalled();
                }
            },
            Qt::QueuedConnection);
        connect(
            &slot.decodePipeline,
            &DecodePipeline::streamBuffering,
            this,
            [this, &slot]() {
                if (isPrimarySlot(slot)) {
                    onStreamBuffering();
                }
            },
            Qt::QueuedConnection);
        connect(
            &slot.decodePipeline,
            &DecodePipeline::streamBuffered,
            this,
            [this, &slot]() {
                if (isPrimarySlot(slot)) {
                    onStreamBuffered();
                }
            },
            Qt::QueuedConnection);
        connect(
            &slot.decodePipeline,
            &DecodePipeline::streamSeekableChanged,
            this,
            [this, &slot](bool seekable) {
                if (isPrimarySlot(slot)) {
                    onStreamSeekable(seekable);
                }
            },
            Qt::QueuedConnection);

        connect(
            &slot.decodePipeline,
            &DecodePipeline::bufferProgressChanged,
            this,
            [this, &slot](qreal progress) {
                if (!isPrimarySlot(slot)) {
                    return;
                }
                if (!qFuzzyCompare(currentBufferProgress, progress)) {
                    currentBufferProgress = progress;
                    Q_EMIT q->bufferProgressChanged(progress);
                }
            },
            Qt::QueuedConnection);
    }
}

DragonPlayer::DragonPlayer(QObject *parent)
    : DragonPlayer(DragonAudioOutput::Backend::Auto, parent)
{
}

DragonPlayer::DragonPlayer(DragonAudioOutput::Backend requestedBackend, QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<DragonFftFrame>();

    // avoid warnings about mismatched threads if a non-main thread is the first caller of k18n code
    std::ignore = KLocalizedString::languages();

    d = std::make_unique<DragonPlayerPrivate>(this, requestedBackend);
    d->init();

    connect(this, &DragonPlayer::playbackStateChanged, this, [](PlaybackState newState, PlaybackState oldState) {
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
    delete d->audioOutput;
    d->audioOutput = nullptr;
}

DragonAudioOutput *DragonPlayer::audioOutput() const
{
    return d->audioOutput;
}

QString DragonPlayer::streamName() const
{
    if (const auto *sink = d->playbackPipeline()) {
        return sink->streamName();
    }
    return {};
}

QUrl DragonPlayer::source() const
{
    return d->currentSource;
}

DragonSourceOptions DragonPlayer::sourceOptions() const
{
    return d->currentSourceOptions;
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
bool DragonPlayer::isSeekable() const
{
    return d->currentSeekable;
}
bool DragonPlayer::seekable() const
{
    return isSeekable();
}
qreal DragonPlayer::bufferProgress() const
{
    return d->currentBufferProgress;
}

QCoro::Task<void> DragonPlayerPrivate::startLoad(PipelineSlot &slot, QUrl source, uint64_t generation)
{
    auto aliveGuard = this->aliveGuard;

    if (generation != loadGeneration) {
        qCDebug(dragonMediaBackendPlayer) << "startLoad superseded before start (generation" << generation << "!= current" << loadGeneration << "), discarding";
        co_return;
    }

    auto result = co_await slot.decodePipeline.initializeSession(source);

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
        if (audioOutput && audioOutput->playbackPipeline()) {
            // Any armed crossfade pointed at a track that failed to load:
            // end the fade and tear the sink down the same way a hard switch
            // failure would.
            fadingSlot().decodePipeline.stopSession();
            teardownPipeline();
            for (auto &slot : m_slots) {
                invalidateSlotContent(slot);
                slot.audioPipe.consumer().drain();
                slot.audioPipe.setProducerDone(false);
            }
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

    if (!currentIsLocal && slot.decodePipeline.streamIsSeekable()) {
        currentSeekable = true;
        Q_EMIT q->seekableChanged(true);
    }

    setStatus(DragonPlayer::MediaStatus::LoadedMedia);

    applyRequestedState(result.sampleRate, result.channels, requestedPlaybackState);

    co_return;
}

void DragonPlayer::setSource(const QUrl &source)
{
    setSource(source, DragonSourceOptions{});
}

void DragonPlayer::setSource(const QUrl &source, const DragonSourceOptions &options)
{
    qCDebug(dragonMediaBackendPlayer) << "setSource(" << source.toString() << ", crossfadeDuration=" << options.crossfadeDuration().count() << "ms)";

    bool playRequestedReload = d->playRequestedReload;
    d->playRequestedReload = false;

    if (!playRequestedReload && d->currentSource == source && d->currentSourceOptions == options && source.isValid()) {
        const bool hasLoadedMedia = d->currentStatus == MediaStatus::LoadedMedia || d->currentStatus == MediaStatus::BufferedMedia
            || d->currentStatus == MediaStatus::StalledMedia || d->currentStatus == MediaStatus::EndOfMedia;

        const bool isSettled = d->currentPlaybackState == PlaybackState::StoppedState && hasLoadedMedia;

        if (!isSettled) {
            qCDebug(dragonMediaBackendPlayer) << "setSource(sameUrl) early return";
            d->requestedPlaybackState = PlaybackState::StoppedState;
            stop();
            return;
        }

        qCDebug(dragonMediaBackendPlayer) << "setSource(sameUrl) while stopped with loaded media: reloading";
    }

    auto *sink = d->playbackPipeline();
    const bool wasPlaying = (d->currentPlaybackState == PlaybackState::PlayingState);
    const bool wasPaused = (d->currentPlaybackState == PlaybackState::PausedState);
    const bool wasPlayingOrPaused = wasPlaying || wasPaused;

    const auto crossfadeDuration = options.crossfadeDuration();
    const bool isCrossfadeRunning = sink && sink->isCrossfading();
    const bool shouldCrossfade = !isCrossfadeRunning && wasPlaying && sink && sink->isDeviceOpen() && !source.isEmpty() && (crossfadeDuration > 0ms);

    {
        std::scoped_lock lock(d->m_pendingTracksMutex);
        d->m_pendingTracks.clear();
    }

    if (shouldCrossfade) {
        // Rotate the 2 slots so the outgoing primary becomes the fading slot and the idle slot becomes the new primary slot.
        auto &outgoing = d->primarySlot();
        auto &incoming = d->fadingSlot();

        d->invalidateSlotContent(incoming);
        incoming.decodePipeline.stopSession();
        if (!sink->isFadingPipe(&incoming.audioPipe) && !sink->isPrimaryPipe(&incoming.audioPipe)) {
            incoming.audioPipe.consumer().drain();
        }
        incoming.audioPipe.setProducerDone(false);

        std::ignore = d->rotateSlots();

        const auto epoch = sink->armCrossfade(&outgoing.audioPipe, &incoming.audioPipe, crossfadeDuration, crossfadeDuration);
        incoming.streamEpoch.store(epoch, std::memory_order_release);
        incoming.producerStreamEpoch.store(epoch, std::memory_order_release);

        // Prevent the fading slot from promoting a pre-warmed gapless next track;
        // it keeps decoding until natural EOF or fadingVoiceEndedCallback stops it.
        outgoing.decodePipeline.clearNextSource();
    } else {
        for (auto &slot : d->m_slots) {
            d->invalidateSlotContent(slot);
            slot.decodePipeline.stopSession();
            slot.audioPipe.setProducerDone(false);
        }

        if (sink) {
            sink->abortFadingVoice();
            if (!sink->close()) {
                qCWarning(dragonMediaBackendPlayer) << "setSource: audio callback did not quiesce on stream close";
            }
        }

        if (sink && !source.isEmpty()) {
            const auto fadeIn = isCrossfadeRunning ? 0ms : crossfadeDuration;
            auto *newPrimaryPipe = &d->primarySlot().audioPipe;
            const auto epoch = sink->setPositionOffset(0ms, newPrimaryPipe, fadeIn);
            d->primarySlot().streamEpoch.store(epoch, std::memory_order_release);
            d->primarySlot().producerStreamEpoch.store(epoch, std::memory_order_release);
            for (auto &slot : d->m_slots) {
                if ((!sink->isPrimaryPipe(&slot.audioPipe) || !sink->isDeviceOpen()) && !sink->isFadingPipe(&slot.audioPipe)) {
                    slot.audioPipe.consumer().drain();
                }
            }
        }
    }

    if (sink && d->currentSource != source) {
        const QString priorStreamName = sink->streamName();
        sink->setStreamName(QString());
        if (!priorStreamName.isEmpty()) {
            Q_EMIT streamNameChanged(QString());
        }
    }

    if (!playRequestedReload) {
        const bool keepPlaying = !source.isEmpty() && wasPlaying && (shouldCrossfade || isCrossfadeRunning || (crossfadeDuration > 0ms));
        d->requestedPlaybackState = keepPlaying ? PlaybackState::PlayingState : PlaybackState::StoppedState;
        if (wasPlayingOrPaused && !keepPlaying) {
            d->setPlaybackState(PlaybackState::StoppedState);
        }
    }

    d->currentSource = source;
    d->currentSourceOptions = options;
    d->positionEstimator->resetPosition(0ms);
    d->positionEstimator->freeze();
    d->currentDuration = {};
    d->positionEstimator->setDuration({});
    d->aboutToFinishEmitted = false;
    d->primarySlot().decodePipeline.clearNextSource();
    d->nextSource.clear();
    d->currentSampleRate = 0;
    d->currentChannels = 0;
    d->currentBufferProgress = 0.0;

    if (!playRequestedReload) {
        if (d->currentSource.isEmpty()) {
            ++d->loadGeneration;
            d->teardownPipeline();
            for (auto &slot : d->m_slots) {
                slot.audioPipe.consumer().drain();
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

    const bool isLocal = StreamFactory::isLocalSource(d->currentSource);
    const bool isHttp = d->currentSource.scheme() == QStringLiteral("http") || d->currentSource.scheme() == QStringLiteral("https");

    d->currentIsLocal = isLocal;
    d->currentSeekable = !isHttp;
    Q_EMIT seekableChanged(d->currentSeekable);

    ++d->loadGeneration;

    const uint64_t generation = d->loadGeneration;
    const QUrl sourceToLoad = d->currentSource;
    auto *slotToLoad = &d->primarySlot();
    auto *priv = d.get();
    QMetaObject::invokeMethod(
        priv,
        [priv, slotToLoad, sourceToLoad, generation]() {
            priv->startLoad(*slotToLoad, sourceToLoad, generation);
        },
        Qt::QueuedConnection);
}

void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    qCDebug(dragonMediaBackendPlayer) << "setNextSource(" << nextSource.toString() << ")";
    d->nextSource = nextSource;
    Q_EMIT nextSourceChanged();

    d->decodePipeline().setNextSource(nextSource);
}

void DragonPlayer::setStreamName(const QString &name)
{
    qCDebug(dragonMediaBackendPlayer) << "setStreamName(" << name << ")";

    if (auto *sink = d->playbackPipeline()) {
        if (sink->streamName() == name) {
            return;
        }
        sink->setStreamName(name);
        Q_EMIT streamNameChanged(name);
    }
}

void DragonPlayerPrivate::resolvePendingTrackForSeek()
{
    // A promoted track is queued but not yet audible: the decoder is already
    // on it while the player still reports the old track. The seek's flush
    // drops the pending TrackStart boundary (trackStarted would never fire),
    // so resolve the switch here: the player reports the track the decoder
    // is actually on, and the seek applies within it.
    std::optional<PendingTrack> resolved = std::nullopt;
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

    auto *sink = d->audioOutput ? d->audioOutput->playbackPipeline() : nullptr;
    if (sink && sink->isCrossfading()) {
        // A seek abandons the fade
        d->fadingSlot().decodePipeline.stopSession();
        sink->abortFadingVoice();
    }

    // Hard-hold the producers while the seek re-positions the decoder; the
    // single flush below reopens the gate when it publishes the new stream.
    if (sink) {
        sink->closeGate();
    }

    // requestSeek() waits out an in-flight gapless promotion, and resumes a
    // Finished decoder from EOF so post-EOF tail seeks proceed.
    d->decodePipeline().requestSeek(position);
    d->primarySlot().audioPipe.setProducerDone(false);

    d->resolvePendingTrackForSeek();

    // The pending switch may have changed the reported track (and with it
    // the duration the position must be clamped against).
    upperBound = d->currentDuration ? std::max(*d->currentDuration, 0ms) : 0ms;
    position = std::clamp(position, 0ms, upperBound);

    if (d->audioOutput) {
        // When actively playing, a promoted track resolved by the seek may
        // need a different device format than the track it replaces; without a
        // reopen the new samples would play at the old rate/channel count.
        // In StoppedState or PausedState, setPositionOffset() closes the
        // stream anyway and play() opens at the current format.
        if (d->currentPlaybackState == PlaybackState::PlayingState && d->audioOutput->playbackPipeline()->isDeviceOpen()
            && !d->audioOutput->playbackPipeline()->hasFormat(d->currentSampleRate, d->currentChannels) && d->currentSampleRate > 0) {
            if (!d->audioOutput->playbackPipeline()->close()) {
                d->setError(DragonPlayer::Error::ResourceError, QStringLiteral("Audio callback did not quiesce on stream close"));
                d->reportDeviceOpenFailure();
                return;
            }
            d->audioOutput->playbackPipeline()->open(d->currentSampleRate, d->currentChannels);
            if (!d->audioOutput->playbackPipeline()->isDeviceOpen()) {
                d->reportDeviceOpenFailure();
                return;
            }
        }
        // One flush per seek: either staged for the audio thread (running
        // sink) or run synchronously (paused/closed sink). It abandons any
        // active fade, drains the old samples, publishes the new stream under
        // the returned epoch, and reopens the gate.
        const auto epoch = d->audioOutput->playbackPipeline()->setPositionOffset(position);
        d->primarySlot().streamEpoch.store(epoch, std::memory_order_release);
        d->audioOutput->playbackPipeline()->clearStream();
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
        if (d->currentStatus == MediaStatus::LoadingMedia && d->playbackPipeline() && d->playbackPipeline()->isCrossfading()) {
            d->playbackPipeline()->resume();
            d->setPlaybackState(PlaybackState::PlayingState);
            return;
        }
        if ((d->decodePipeline().isActive() || d->decodePipeline().isFinished()) && d->audioOutput && d->audioOutput->playbackPipeline()->isDeviceOpen()) {
            d->audioOutput->playbackPipeline()->resume();
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

    if (!d->decodePipeline().isActive() && !d->decodePipeline().isFinished() && !d->currentSource.isEmpty()) {
        qCDebug(dragonMediaBackendPlayer) << "play() decoder not active, restarting decode pipeline";
        d->playRequestedReload = true;
        d->requestedPlaybackState = PlaybackState::PlayingState;
        setSource(d->currentSource);
        return;
    }

    qCDebug(dragonMediaBackendPlayer) << "play() status is " << d->currentStatus << ", starting audio synchronously";

    if (d->audioOutput && d->currentSampleRate > 0
        && (!d->audioOutput->playbackPipeline()->isDeviceOpen() || !d->audioOutput->playbackPipeline()->hasFormat(d->currentSampleRate, d->currentChannels))) {
        d->audioOutput->playbackPipeline()->open(d->currentSampleRate, d->currentChannels);
    }
    if (d->audioOutput && !d->audioOutput->playbackPipeline()->isDeviceOpen()) {
        d->reportDeviceOpenFailure();
        return;
    }
    if (d->audioOutput) {
        d->audioOutput->playbackPipeline()->reopenIfIdle();
        d->audioOutput->playbackPipeline()->resume(); // pipewire needs a kick here
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
        if (d->currentPlaybackState == PlaybackState::PlayingState) {
            if (auto *sink = d->playbackPipeline()) {
                sink->pause();
            }
            d->setPlaybackState(PlaybackState::PausedState);
        }
        return;
    }

    if (d->currentPlaybackState == PlaybackState::StoppedState) {
        qCDebug(dragonMediaBackendPlayer) << "pause() while stopped, ignored";
        return;
    }

    d->requestedPlaybackState = PlaybackState::PausedState;

    if (d->audioOutput) {
        d->audioOutput->playbackPipeline()->pause();
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
        d->decodePipeline().setNextSource(QUrl());
    }

    {
        std::scoped_lock lock(d->m_pendingTracksMutex);
        d->m_pendingTracks.clear();
    }

    if (d->currentStatus == MediaStatus::LoadingMedia) {
        qCDebug(dragonMediaBackendPlayer) << "stop() during LoadingMedia, letting in-flight load continue";
        auto *sink = d->playbackPipeline();
        if (sink && sink->isCrossfading()) {
            d->invalidateSlotContent(d->fadingSlot());
            d->fadingSlot().decodePipeline.stopSession();
            sink->abortFadingVoice();
            d->fadingSlot().audioPipe.consumer().drain();
            d->fadingSlot().audioPipe.setProducerDone(false);
        }
        bool closeFailed = false;
        if (sink && sink->isDeviceOpen()) {
            closeFailed = !sink->close();
            if (closeFailed) {
                qCWarning(dragonMediaBackendPlayer) << "stop: audio callback did not quiesce on stream close";
            }
        }
        if (sink && (closeFailed || sink->isFlushPending())) {
            const auto epoch = sink->setPositionOffset(0ms);
            d->primarySlot().streamEpoch.store(epoch, std::memory_order_release);
        }
        d->setPlaybackState(PlaybackState::StoppedState);
        d->positionEstimator->freeze();
        if (d->positionEstimator->position() != 0ms) {
            d->positionEstimator->seek(0ms);
        }
        return;
    }

    d->teardownPipeline();

    for (auto &slot : d->m_slots) {
        d->invalidateSlotContent(slot);
        slot.decodePipeline.stopSession();
        slot.audioPipe.setProducerDone(false);
    }

    if (d->audioOutput) {
        for (auto &slot : d->m_slots) {
            slot.audioPipe.consumer().drain();
        }
    }

    d->setPlaybackState(PlaybackState::StoppedState);

    if (d->positionEstimator->position() != 0ms) {
        d->positionEstimator->seek(0ms);
    }

    if (d->currentStatus != MediaStatus::NoMedia && d->currentStatus != MediaStatus::InvalidMedia) {
        d->setStatus(MediaStatus::LoadedMedia);
    }
}
