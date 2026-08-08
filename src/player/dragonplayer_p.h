/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "decoder/dragondecodepipeline.h"
#include "sink/dragonaudiosink.h"
#include <DragonMultimedia/dragonaudiooutput.h>
#include <DragonMultimedia/dragonplayer.h>

#include "dragonpipe.h"
#include "fft/dragonfftblock.h"

#include <QObject>
#include <QTimer>
#include <QUrl>

#include "dragonstdfloat_compat.h"
#include <QCoroTask>
#include <atomic>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

class DragonDiagnostics;
class DragonAudioOutput;

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

    void onGaplessTransition(const QUrl &newSource, int sampleRate, int channels, qint64 durationMs);

    void onStreamStalled();
    void onStreamBuffering();
    void onStreamBuffered();

private:
    void applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent);

    void writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st);

    QCoro::Task<void> startLoad(QUrl source, uint64_t generation);

    void setPlaybackState(DragonPlayer::PlaybackState state);
    void setStatus(DragonPlayer::MediaStatus status);
    void setError(DragonPlayer::Error error, const QString &message = {});

    void stopPipeline();

    DragonDecodePipeline decodePipeline;

    DragonPipe<std::float32_t> audioPipe;

    QUrl currentSource;
    QUrl nextSource;
    DragonPlayer::PlaybackState currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    DragonPlayer::MediaStatus currentStatus = DragonPlayer::MediaStatus::NoMedia;
    DragonPlayer::Error currentError = DragonPlayer::Error::NoError;
    QString currentErrorString;
    DragonPlayer::PlaybackState requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    qint64 currentDuration = 0;
    bool currentSeekable = false;
    bool currentIsLocal = false;
    int currentSampleRate = 0;
    int currentChannels = 0;
    qreal currentBufferProgress = 1.0;
    DragonAudioOutput::Backend requestedBackend = DragonAudioOutput::Backend::Auto;

    std::shared_ptr<AliveGuard> aliveGuard;

    qint64 undoPosition = 0;

    QTimer *positionTimer = nullptr;
    qint64 currentPosition = 0;

    bool playRequestedReload = false;

    uint64_t loadGeneration = 0;

    int32_t prefinishMark = 0;
    bool aboutToFinishEmitted = false;
    bool inGaplessSetSource = false;
};
