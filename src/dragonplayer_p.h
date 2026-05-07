/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragondecodepipeline.h"
#include "dragonfftpipeline.h"
#include <dragonaudiooutput.h>
#include <dragondecoder.h>
#include <dragonsdl/dragonplayer.h>

#include <LockFreeSpscQueue.h>

#include <SDL3/SDL_audio.h>

#include "dragonsdl_logging.h"
#include <QMetaObject>
#include <QTimer>

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

class DragonPlayer::DragonPlayerPrivate
{
    static constexpr size_t kBufferCapacity = 65536;

    friend class DragonDiagnostics;

public:
    explicit DragonPlayerPrivate(DragonPlayer *player)
        : q(player)
        , decodePipeline(player)
    {
        audioBuffer.resize(kBufferCapacity);
        audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(audioBuffer));

        audioOutput = std::make_unique<DragonAudioOutput>();
        audioOutput->setQueue(audioQueue.get());
        audioOutput->setFftQueue(nullptr);

        connect(audioOutput.get(), &DragonAudioOutput::errorOccurred, q, [this](const QString &) {
            setError(DragonPlayer::Error::ResourceError);
        });

        connect(audioOutput.get(), &DragonAudioOutput::volumeChanged, q, &DragonPlayer::volumeChanged);

        positionTimer = new QTimer(q);
        positionTimer->setInterval(100);
        QObject::connect(positionTimer, &QTimer::timeout, q, [this]() {
            Q_EMIT q->positionChanged(position());
        });

        wirePipelineCallbacks();
        wireFftCallbacks();
    }

    void wirePipelineCallbacks()
    {
        decodePipeline.setCallbacks(
            [this](int sr, int ch, bool isGapless) {
                onFormatReady(sr, ch, isGapless);
            },
            [this](int64_t dur) {
                setDuration(dur);
            },
            [this](auto samples, const std::stop_token &st) {
                writeToQueues(samples, st);
            },
            [this](const QString &) {
                setError(currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError);
            },
            [this](bool hadFatalError, bool) {
                onDecodeFinished(hadFatalError);
            },
            [this](const QUrl &newSource) {
                onGaplessTransition(newSource);
            });
    }

    void wireFftCallbacks()
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

    void onFormatReady(int sampleRate, int channels, bool isGapless)
    {
        qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels;
        currentSampleRate = sampleRate;
        currentChannels = channels;

        bool audioAlreadyRunning = isGapless && audioOutput->hasFormat(sampleRate, channels);

        audioOutput->setPositionOffset(currentPosition);

        fftPipeline.setSampleRate(sampleRate);

        setStatus(DragonPlayer::MediaStatus::LoadedMedia);

        if (requestedPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
            requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
            if (!audioAlreadyRunning) {
                audioOutput->start(sampleRate, channels);
            }
            setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
        } else if (requestedPlaybackState == DragonPlayer::PlaybackState::PausedState) {
            requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
            if (!audioAlreadyRunning) {
                audioOutput->start(sampleRate, channels);
            }
            if (audioOutput) {
                audioOutput->pause();
            }
            setPlaybackState(DragonPlayer::PlaybackState::PausedState);
        } else {
            if (isGapless && currentPlaybackState == DragonPlayer::PlaybackState::PlayingState && !audioAlreadyRunning) {
                audioOutput->start(sampleRate, channels);
            }
        }
    }

    void onGaplessTransition(const QUrl &newSource)
    {
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

    void onDecodeFinished(bool hadFatalError)
    {
        if (hadFatalError) {
            QMetaObject::invokeMethod(
                q,
                [this]() {
                    setError(currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError);
                    setStatus(DragonPlayer::MediaStatus::InvalidMedia);
                    setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
                },
                Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(
                q,
                [this]() {
                    if (audioOutput) {
                        audioOutput->stop();
                        audioOutput->reset();
                    }
                    fftPipeline.stop();
                    setStatus(DragonPlayer::MediaStatus::EndOfMedia);
                    setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
                },
                Qt::QueuedConnection);
        }
    }

    ~DragonPlayerPrivate()
    {
        stopPipeline();
    }

    void setSource(const QUrl &source)
    {
        qCDebug(dragonsdlPlayer) << "setSource(" << source.toString() << ")";

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        if (currentFftMode == DragonPlayer::FftMode::Off) {
            fftPipeline.stop();
            fftPipeline.teardown();
            fftQueue.reset();
            fftBuffer.clear();
            fftBuffer.shrink_to_fit();
            if (audioOutput) {
                audioOutput->setFftQueue(nullptr);
            }
        } else {
            fftPipeline.ensureInfrastructure(&fftBuffer, audioOutput->fftCv());

            fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));
            if (audioOutput) {
                audioOutput->setFftQueue(fftQueue.get());
            }

            fftPipeline.restartWithNewQueue(fftQueue.get(), audioOutput->fftCv());
        }

        audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(audioBuffer));
        audioOutput->setQueue(audioQueue.get());

        currentSource = source;
        currentPosition = 0;
        currentDuration = 0;
        nextSource.clear();
        currentSampleRate = 0;
        currentChannels = 0;

        Q_EMIT q->sourceChanged();
        Q_EMIT q->nextSourceChanged();

        if (source.isEmpty()) {
            setStatus(DragonPlayer::MediaStatus::NoMedia);
            setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
            return;
        }

        if (currentPlaybackState != DragonPlayer::PlaybackState::StoppedState) {
            setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
        } else {
            Q_EMIT q->playbackStateChanged(DragonPlayer::PlaybackState::StoppedState);
            Q_EMIT q->stopped();
        }

        requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;

        setError(DragonPlayer::Error::NoError);
        setStatus(DragonPlayer::MediaStatus::LoadingMedia);

        const bool isLocal = source.isLocalFile();
        currentIsLocal = isLocal;
        currentSeekable = isLocal;
        Q_EMIT q->seekableChanged(currentSeekable);

        ++currentDecoderGeneration;
        decodePipeline.setSource(source, currentDecoderGeneration);
    }

    void setNextSource(const QUrl &next)
    {
        nextSource = next;
        Q_EMIT q->nextSourceChanged();

        decodePipeline.setNextSource(next, currentDecoderGeneration);
    }

    void stopPipeline()
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

    void play()
    {
        if (currentSource.isEmpty()) {
            return;
        }

        if (currentPlaybackState == DragonPlayer::PlaybackState::PlayingState) {
            return;
        }

        if (currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
            if (audioOutput) {
                audioOutput->resume();
            }
            setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
            return;
        }

        if (currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
            requestedPlaybackState = DragonPlayer::PlaybackState::PlayingState;
            return;
        }

        if (currentStatus == DragonPlayer::MediaStatus::EndOfMedia) {
            setSource(currentSource);
            requestedPlaybackState = DragonPlayer::PlaybackState::PlayingState;
            return;
        }

        if (audioOutput && !audioOutput->isDeviceOpen() && currentSampleRate > 0) {
            audioOutput->start(currentSampleRate, currentChannels);
        }
        setPlaybackState(DragonPlayer::PlaybackState::PlayingState);
    }

    void pause()
    {
        if (currentPlaybackState == DragonPlayer::PlaybackState::PausedState) {
            return;
        }

        if (currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
            requestedPlaybackState = DragonPlayer::PlaybackState::PausedState;
            return;
        }

        if (audioOutput) {
            audioOutput->pause();
        }
        setPlaybackState(DragonPlayer::PlaybackState::PausedState);
    }

    void stop()
    {
        qCDebug(dragonsdlPlayer) << "stop()";

        if (currentStatus == DragonPlayer::MediaStatus::LoadingMedia) {
            requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
            return;
        }

        if (decodePipeline.isActive() && audioOutput && !audioOutput->isDeviceOpen()) {
            qCDebug(dragonsdlPlayer) << "stop() decoder exists but audio not open yet, ignoring stale stop";
            return;
        }

        decodePipeline.stop();
        ++currentDecoderGeneration;

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        fftPipeline.stop();

        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);

        if (currentStatus != DragonPlayer::MediaStatus::LoadedMedia) {
            currentStatus = DragonPlayer::MediaStatus::LoadedMedia;
        }
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::LoadedMedia);
    }

    void writeToQueues(std::span<const std::float32_t> pcm, const std::stop_token &st)
    {
        if (pcm.empty()) {
            return;
        }

        writeToQueueWithBackpressure(*audioQueue, pcm, st);
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

    [[nodiscard]] bool isAudioActive() const
    {
        return audioOutput ? audioOutput->isDeviceOpen() : false;
    }

    void setPlaybackState(DragonPlayer::PlaybackState state)
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

    void setStatus(DragonPlayer::MediaStatus status)
    {
        if (currentStatus == status) {
            return;
        }
        currentStatus = status;
        Q_EMIT q->statusChanged(status);

        if (status == DragonPlayer::MediaStatus::InvalidMedia) {
            setError(DragonPlayer::Error::FormatError);
        }
    }

    void setError(DragonPlayer::Error error)
    {
        if (currentError == error) {
            return;
        }
        currentError = error;
        Q_EMIT q->errorChanged(error);

        if (error != DragonPlayer::Error::NoError) {
            setStatus(DragonPlayer::MediaStatus::InvalidMedia);
        }
    }

    void setDuration(const int64_t durationMs)
    {
        if (currentDuration == durationMs) {
            return;
        }
        currentDuration = durationMs;
        Q_EMIT q->durationChanged(durationMs);
    }

    [[nodiscard]] float volume() const
    {
        return currentVolume;
    }

    void setVolume(float gain)
    {
        currentVolume = gain;
        if (audioOutput) {
            audioOutput->setVolume(gain);
        }
    }

    bool muted() const
    {
        return currentMuted;
    }

    void setMuted(bool m)
    {
        if (currentMuted == m) {
            return;
        }
        currentMuted = m;
        if (audioOutput) {
            audioOutput->setMuted(m);
        }
        Q_EMIT q->mutedChanged(m);
    }

    DragonPlayer::FftMode fftMode() const
    {
        return currentFftMode;
    }

    void setFftMode(DragonPlayer::FftMode mode)
    {
        if (currentFftMode == mode) {
            return;
        }

        const bool wasOn = (currentFftMode != DragonPlayer::FftMode::Off);
        const bool nowOn = (mode != DragonPlayer::FftMode::Off);
        currentFftMode = mode;

        if (!wasOn && nowOn) {
            fftPipeline.ensureInfrastructure(&fftBuffer, audioOutput->fftCv());

            if (!fftQueue) {
                fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));
            }

            if (audioOutput) {
                audioOutput->setFftQueue(fftQueue.get());
            }

            fftPipeline.setQueue(fftQueue.get());

            fftPipeline.setMode(mode);

            if (fftPipeline.isRunning()) {
                fftPipeline.restartWithNewQueue(fftQueue.get(), audioOutput->fftCv());
            } else {
                fftPipeline.start();
            }
        } else if (wasOn && !nowOn) {
            if (audioOutput) {
                audioOutput->setFftQueue(nullptr);
            }
            fftPipeline.setMode(mode);
        } else if (nowOn && fftPipeline.hasInfrastructure()) {
            fftPipeline.setMode(mode);
        }

        Q_EMIT q->fftModeChanged(mode);
    }

    int64_t position() const
    {
        if (audioOutput && audioOutput->isDeviceOpen()) {
            return audioOutput->positionMs();
        }
        return currentPosition;
    }

    void setPosition(int64_t posMs)
    {
        posMs = std::clamp(posMs, int64_t{0}, std::max(currentDuration, int64_t{0}));
        currentPosition = posMs;

        decodePipeline.requestSeek(posMs);

        if (audioOutput) {
            audioOutput->setPositionOffset(posMs);
            audioOutput->clearStream();
        }
        Q_EMIT q->positionChanged(posMs);
    }

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
