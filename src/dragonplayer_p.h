/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonaudiooutput.h"
#include "dragondecodepipeline.h"
#include "dragonfftpipeline.h"
#include <dragonsdl/dragonplayer.h>

#include "dragonpipe.h"

#include <QObject>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <memory>
#include <span>
#include <stdfloat>
#include <stop_token>
#include <vector>

class DragonDiagnostics;

struct AliveGuard {
    std::atomic<bool> alive{true};
};

class DragonPlayerPrivate : public QObject
{
    Q_OBJECT
public:
    explicit DragonPlayerPrivate(DragonPlayer *player);

private:
    friend class DragonDiagnostics;
    friend class DragonPlayer;

    void init();

    DragonPlayer *q = nullptr;

private Q_SLOTS:
    void onDecodeFinished(const QUrl &source, bool hadFatalError);
    void onDecodeError(const QString &message);

    void onGaplessTransition(const QUrl &newSource, int sampleRate, int channels, qint64 durationMs);

private:
    void applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent);

    void writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st);

    void setPlaybackState(DragonPlayer::PlaybackState state);
    void setStatus(DragonPlayer::MediaStatus status);
    void setError(DragonPlayer::Error error);

    void stopPipeline();

    DragonDecodePipeline decodePipeline;
    DragonFftPipeline fftPipeline;

    DragonPipe<std::float32_t> audioPipe;
    DragonPipe<std::float32_t> fftPipe;

    std::unique_ptr<DragonAudioOutput> audioOutput;

    QUrl currentSource;
    QUrl nextSource;
    DragonPlayer::PlaybackState currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    DragonPlayer::MediaStatus currentStatus = DragonPlayer::MediaStatus::NoMedia;
    DragonPlayer::Error currentError = DragonPlayer::Error::NoError;
    DragonPlayer::PlaybackState requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    int64_t currentDuration = 0;
    float currentVolume = 1.0f;
    bool currentMuted = false;
    bool currentSeekable = false;
    bool currentIsLocal = false;
    int currentSampleRate = 0;
    int currentChannels = 0;
    DragonPlayer::FftMode currentFftMode = DragonPlayer::FftMode::Off;
    double currentBufferProgress = 1.0;

    std::shared_ptr<AliveGuard> aliveGuard;

    int64_t undoPosition = 0;

    QTimer *positionTimer = nullptr;
    int64_t currentPosition = 0;
};