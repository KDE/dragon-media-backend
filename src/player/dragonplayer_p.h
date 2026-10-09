/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "decoder/dragondecodepipeline.h"
#include "sink/dragonaudiosink.h"
#include <dragonaudiooutput.h>
#include <dragonplayer.h>

#include "dragonpipe.h"

#include <QObject>
#include <QTimer>
#include <QUrl>

#include <QCoroTask>
#include <QHash>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

using namespace std::chrono_literals;

class DragonDiagnostics;
class DragonAudioOutput;
class DragonPositionEstimator;

struct AliveGuard {
    std::atomic<bool> alive{true};
};

class DragonPlayerPrivate : public QObject
{
    Q_OBJECT
public:
    explicit DragonPlayerPrivate(DragonPlayer *player, DragonAudioOutput::Backend requestedBackend = DragonAudioOutput::Backend::Auto);

private:
    friend class DragonDiagnostics;
    friend class DragonPlayer;
    friend class DragonSpectrumAnalyzer;
    friend class DragonSpectrumAnalyzerPrivate;

    void init();

    DragonAudioSink *audioSink() const;

    DragonPlayer *q = nullptr;

    DragonAudioOutput *audioOutput = nullptr;

private Q_SLOTS:
    void onDecodeFinished(const QUrl &source, bool hadFatalError);
    void onDecodeError(const QString &message);

    void onSinkTrackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration);
    void onSinkFormatChangeRequired(int sampleRate, int channels);

    void onStreamStalled();
    void onStreamBuffering();
    void onStreamBuffered();
    void onStreamSeekable(bool seekable);

private:
    bool streamStateTransitionsAllowed() const;

    void applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent);
    void reportDeviceOpenFailure();

    void writeToQueues(std::span<const float> pcm, const std::stop_token &st);

    bool onPipelineBoundary(const DragonDecodePipeline::BoundaryEvent &event);

    struct PendingTrack {
        QUrl url;
        int sampleRate = 0;
        int channels = 0;
        std::optional<std::chrono::milliseconds> duration;
        bool seekable = false;
    };
    QHash<std::uint32_t, PendingTrack> m_pendingTracks;
    mutable std::mutex m_pendingTracksMutex;
    std::atomic<std::uint32_t> m_trackSerial{0};

    // Applies a pending track switch to the player's reported state (source,
    // duration, seekability, format). Used both at play time (the sink
    // crossed a TrackStart boundary) and at seek time (the seek's flush
    // would drop the boundary, so the switch is resolved eagerly).
    void applyPendingTrack(const PendingTrack &pending);

    // Resolves the pending track switch, if any, from within setPosition():
    // the decoder is already on the promoted track while the player still
    // reports the old one, and the seek's flush drops the pending boundary.
    // Must run after requestSeek() (which waits out an in-flight promotion
    // and thereby guarantees the pending entry exists) and before the flush
    // is armed.
    void resolvePendingTrackForSeek();

    // The stream epoch the decode thread's producer last observed before a
    // write (see DragonAudioSink::streamEpoch()). Decode-thread confined
    // writer; read on the decode thread when recording boundaries. A
    // TrackStart boundary is stamped with it so boundaries of a superseded
    // stream are discarded instead of firing in the new one.
    std::atomic<std::uint64_t> m_producerStreamEpoch{0};

    void invalidateQueuedContent();

    QCoro::Task<void> startLoad(QUrl source, uint64_t generation);

    void setPlaybackState(DragonPlayer::PlaybackState state);
    void setStatus(DragonPlayer::MediaStatus status);
    void setError(DragonPlayer::Error error, const QString &message = {});

    void stopPipeline();

    DragonDecodePipeline decodePipeline;

    DragonPipe<float> audioPipe;

    QUrl currentSource;
    QUrl nextSource;
    DragonPlayer::PlaybackState currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    DragonPlayer::MediaStatus currentStatus = DragonPlayer::MediaStatus::NoMedia;
    DragonPlayer::Error currentError = DragonPlayer::Error::NoError;
    QString currentErrorString;
    DragonPlayer::PlaybackState requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    std::optional<std::chrono::milliseconds> currentDuration{};
    bool currentSeekable = false;
    bool currentIsLocal = false;
    int currentSampleRate = 0;
    int currentChannels = 0;
    qreal currentBufferProgress = 1.0;
    DragonAudioOutput::Backend requestedBackend = DragonAudioOutput::Backend::Auto;

    std::shared_ptr<AliveGuard> aliveGuard;

    DragonPositionEstimator *positionEstimator = nullptr;

    bool playRequestedReload = false;

    uint64_t loadGeneration = 0;

    // Bumped on seek and track change so the decode thread's producer can
    // discard queued/in-flight stale audio itself instead of relying on the
    // audio sink's consumer-side drain to win a race against fresh samples.
    std::atomic<uint64_t> contentGeneration{0};

    std::chrono::milliseconds prefinishMark{2000ms};
    bool aboutToFinishEmitted = false;
};
