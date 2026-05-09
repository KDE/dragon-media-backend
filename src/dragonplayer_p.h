/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonaudiooutput.h"
#include "dragondecodepipeline.h"
#include "dragonfftpipeline.h"
#include <dragonsdl/dragonplayer.h>

#include <LockFreeSpscQueue.h>

#include <QTimer>
#include <QUrl>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdfloat>
#include <stop_token>
#include <vector>

class DragonDiagnostics;

struct DragonPlayerPrivate {
    static constexpr size_t kBufferCapacity = 65536;

    friend class DragonDiagnostics;

    void init();

    void onFormatReady(int sampleRate, int channels, bool isGapless);
    void onGaplessTransition(const QUrl &newSource);
    void onDecodeFinished(bool hadFatalError);

    void applyRequestedState(int sampleRate, int channels, bool isGapless, bool audioAlreadyRunning);

    void writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st);

    void setPlaybackState(DragonPlayer::PlaybackState state);
    void setStatus(DragonPlayer::MediaStatus status);
    void setError(DragonPlayer::Error error);

    void stopPipeline();

    void wirePipelineCallbacks();
    void wireFftCallbacks();

    DragonPlayer *q;

    DragonDecodePipeline decodePipeline;
    DragonFftPipeline fftPipeline;

    std::vector<std::float32_t> fftBuffer;
    std::vector<std::float32_t> audioBuffer;
    std::unique_ptr<LockFreeSpscQueue<std::float32_t>> fftQueue;
    std::unique_ptr<LockFreeSpscQueue<std::float32_t>> audioQueue;

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

    uint64_t currentDecoderGeneration = 0;

    int64_t undoPosition = 0;

    QTimer *positionTimer = nullptr;
    int64_t currentPosition = 0;
};
