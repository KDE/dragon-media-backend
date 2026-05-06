/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <dragonaudiooutput.h>
#include <dragondecoder.h>
#include <dragonfftprocessor.h>
#include <dragonradiostream.h>
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

        startDecodeThread();
    }

    ~DragonPlayerPrivate()
    {
        stopPipeline();
    }

    void startDecodeThread()
    {
        decodeThread = std::jthread([this](std::stop_token st) {
            setCurrentThreadName("dragon-decode");
            while (!st.stop_requested()) {
                std::unique_lock lock(decoderMutex);
                decoderCv.wait(lock, [this, &st]() {
                    return activeDecoder != nullptr || st.stop_requested();
                });
                if (st.stop_requested())
                    break;

                DragonDecoder *decoder = activeDecoder.get();
                decodeLoopActive = true;
                lock.unlock();

                decoder->decodeLoop(decodeStopSource.get_token());
                const bool hadFatalError = decoder->hasFatalError();

                lock.lock();
                decodeLoopActive = false;
                const bool wasStopped = decodeStopSource.stop_requested();

                activeDecoder.reset();

                lock.unlock();
                decoderCv.notify_all();

                if (st.stop_requested())
                    break;

                if (wasStopped) {
                    continue;
                }

                if (hadFatalError) {
                    QMetaObject::invokeMethod(
                        q,
                        [this]() {
                            setError(currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError);
                            setStatus(DragonPlayer::MediaStatus::InvalidMedia);
                            setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
                        },
                        Qt::QueuedConnection);
                    continue;
                }

                std::unique_lock plock(decoderMutex);
                if (preWarmedDecoder) {
                    activeDecoder = std::move(preWarmedDecoder);
                    QUrl newSource = nextSource;
                    nextSource.clear();

                    plock.unlock();

                    QMetaObject::invokeMethod(
                        q,
                        [this, newSource]() {
                            currentSource = newSource;
                            currentPosition = 0;
                            currentIsLocal = newSource.isLocalFile();
                            currentSeekable = currentIsLocal;
                            currentDuration = 0;

                            audioOutput->setPositionOffset(0);

                            Q_EMIT q->trackChanged();
                            Q_EMIT q->sourceChanged();
                            Q_EMIT q->nextSourceChanged();
                            Q_EMIT q->seekableChanged(currentSeekable);
                        },
                        Qt::QueuedConnection);
                } else {
                    plock.unlock();

                    QMetaObject::invokeMethod(
                        q,
                        [this]() {
                            if (audioOutput) {
                                audioOutput->stop();
                                audioOutput->reset();
                            }
                            if (fftThread.joinable()) {
                                fftThread.request_stop();
                                fftThread.join();
                            }
                            setStatus(DragonPlayer::MediaStatus::EndOfMedia);
                            setPlaybackState(DragonPlayer::PlaybackState::StoppedState);
                        },
                        Qt::QueuedConnection);
                }
            }
        });
    }

    void ensureFftInfrastructure()
    {
        if (fftProcessor) {
            return;
        }

        fftBuffer.resize(kBufferCapacity);

        fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));

        audioOutput->setFftQueue(fftQueue.get());

        fftProcessor = std::make_unique<DragonFftProcessor>();
        fftProcessor->setQueue(fftQueue.get());
        fftProcessor->setWaitCv(audioOutput->fftCv());
        fftProcessor->setFftMode(currentFftMode);

        fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            QMetaObject::invokeMethod(
                q,
                [this, f = std::move(frame)]() mutable {
                    Q_EMIT q->fftFrameReady(f);
                },
                Qt::QueuedConnection);
        });
    }

    void startFftThread()
    {
        if (fftThread.joinable()) {
            return;
        }
        fftThread = std::jthread([this](std::stop_token st) {
            setCurrentThreadName("dragon-fft");
            fftProcessor->processLoop(std::move(st));
        });
    }

    std::unique_ptr<DragonDecoder> createDecoder(const QUrl &source, bool isGapless, uint64_t generation)
    {
        const bool isLocal = source.isLocalFile();

        if (!isLocal) {
            if (radioStream) {
                radioStream->stop();
                radioStream.reset();
            }
            radioStream = std::make_unique<DragonRadioStream>();
            radioStream->setUrl(source);

            connect(radioStream.get(), &DragonRadioStream::errorOccurred, q, [this](const QString &) {
                setError(DragonPlayer::Error::NetworkError);
            });

            connect(radioStream.get(), &DragonRadioStream::metadataReady, q, [this](const DragonIcyMetadata &metadata) {
                Q_EMIT q->currentPlayingForRadiosChanged(metadata);
            });

            radioStream->start();
        } else if (radioStream) {
            radioStream->stop();
            radioStream.reset();
        }

        DragonDecoder::ReadCallback readCb;
        if (!isLocal) {
            readCb = [this](const std::span<uint8_t> buf) -> int {
                return radioStream ? radioStream->read(buf, decodeStopSource.get_token()) : -1;
            };
        }

        auto decoder = std::make_unique<DragonDecoder>(std::move(readCb), isLocal ? source.toLocalFile() : QString{});

        decoder->setSamplesCallback([this](std::span<const std::float32_t> data, int, int) {
            writeToQueues(data);
        });

        connect(
            decoder.get(),
            &DragonDecoder::formatReady,
            q,
            [this, generation, isGapless](int sampleRate, int channels) {
                if (currentDecoderGeneration != generation) {
                    qCDebug(dragonsdlPlayer) << "ignoring stale formatReady (gen" << generation << "!= current" << currentDecoderGeneration << ")";
                    return;
                }
                qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels;
                currentSampleRate = sampleRate;
                currentChannels = channels;

                bool audioAlreadyRunning = isGapless && audioOutput->hasFormat(sampleRate, channels);

                audioOutput->setPositionOffset(currentPosition);
                if (fftProcessor) {
                    fftProcessor->setSampleRate(sampleRate);
                }
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
            },
            Qt::QueuedConnection);

        connect(
            decoder.get(),
            &DragonDecoder::durationChanged,
            q,
            [this](int64_t durationMs) {
                setDuration(durationMs);
            },
            Qt::QueuedConnection);

        connect(
            decoder.get(),
            &DragonDecoder::streamError,
            q,
            [this](const QString &msg) {
                qCDebug(dragonsdlPlayer) << "Decoder error:" << msg;
                setError(currentIsLocal ? DragonPlayer::Error::FormatError : DragonPlayer::Error::NetworkError);
            },
            Qt::QueuedConnection);

        return decoder;
    }

    void setSource(const QUrl &source)
    {
        qCDebug(dragonsdlPlayer) << "setSource(" << source.toString() << ")";

        {
            std::lock_guard lock(decoderMutex);
            preWarmedDecoder.reset();
        }
        if (preWarmThread.joinable()) {
            preWarmThread.request_stop();
            preWarmThread.join();
        }

        decodeStopSource.request_stop();

        {
            std::unique_lock lock(decoderMutex);
            decoderCv.wait(lock, [this]() {
                return !decodeLoopActive;
            });
            activeDecoder.reset();
        }
        ++currentDecoderGeneration;

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        if (currentFftMode == DragonPlayer::FftMode::Off) {
            if (fftThread.joinable()) {
                fftThread.request_stop();
                fftThread.join();
            }
            fftProcessor.reset();
            fftQueue.reset();
            fftBuffer.clear();
            fftBuffer.shrink_to_fit();
            if (audioOutput) {
                audioOutput->setFftQueue(nullptr);
            }
        } else {
            ensureFftInfrastructure();

            if (fftThread.joinable()) {
                fftThread.request_stop();
                fftThread.join();
            }

            fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));
            if (audioOutput) {
                audioOutput->setFftQueue(fftQueue.get());
            }
            fftProcessor->setQueue(fftQueue.get());
            fftProcessor->setWaitCv(audioOutput->fftCv());
            fftProcessor->reset();

            startFftThread();
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

        auto decoder = createDecoder(source, false, currentDecoderGeneration);
        if (!decoder) {
            setError(DragonPlayer::Error::FormatError);
            setStatus(DragonPlayer::MediaStatus::InvalidMedia);
            return;
        }

        {
            std::lock_guard lock(decoderMutex);
            activeDecoder = std::move(decoder);
        }

        decodeStopSource = std::stop_source{};
        decoderCv.notify_one();
    }

    void setNextSource(const QUrl &next)
    {
        nextSource = next;
        Q_EMIT q->nextSourceChanged();

        {
            std::lock_guard lock(decoderMutex);
            preWarmedDecoder.reset();
        }
        if (preWarmThread.joinable()) {
            preWarmThread.request_stop();
            preWarmThread.join();
        }

        if (next.isEmpty() || !next.isLocalFile()) {
            return;
        }

        preWarmThread = std::jthread([this, next](std::stop_token st) {
            setCurrentThreadName("dragon-prewarm");
            auto decoder = createDecoder(next, true, currentDecoderGeneration);
            if (st.stop_requested()) {
                return;
            }
            if (decoder) {
                std::lock_guard lock(decoderMutex);
                if (!st.stop_requested()) {
                    preWarmedDecoder = std::move(decoder);
                }
            }
        });
    }

    void stopPipeline()
    {
        qCDebug(dragonsdlPlayer) << "stopPipeline() full teardown";

        decodeStopSource.request_stop();

        decodeThread.request_stop();

        fftThread.request_stop();

        if (preWarmThread.joinable()) {
            preWarmThread.request_stop();
        }

        decoderCv.notify_all();

        if (decodeThread.joinable()) {
            decodeThread.join();
        }
        if (fftThread.joinable()) {
            fftThread.join();
        }
        if (preWarmThread.joinable()) {
            preWarmThread.join();
        }

        {
            std::lock_guard lock(decoderMutex);
            activeDecoder.reset();
            preWarmedDecoder.reset();
            decodeLoopActive = false;
        }

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        if (radioStream) {
            radioStream->stop();
            radioStream.reset();
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

        {
            std::lock_guard lock(decoderMutex);
            if (activeDecoder && audioOutput && !audioOutput->isDeviceOpen()) {
                qCDebug(dragonsdlPlayer) << "stop() decoder exists but audio not open yet, ignoring stale stop";
                return;
            }
        }

        {
            std::lock_guard lock(decoderMutex);
            preWarmedDecoder.reset();
        }
        if (preWarmThread.joinable()) {
            preWarmThread.request_stop();
            preWarmThread.join();
        }

        if (decodeStopSource.stop_possible()) {
            decodeStopSource.request_stop();
        }
        {
            std::unique_lock lock(decoderMutex);
            decoderCv.wait(lock, [this]() {
                return !decodeLoopActive;
            });
            activeDecoder.reset();
        }
        ++currentDecoderGeneration;

        if (radioStream) {
            radioStream->stop();
            radioStream.reset();
        }

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        if (fftThread.joinable()) {
            fftThread.request_stop();
            fftThread.join();
        }

        setPlaybackState(DragonPlayer::PlaybackState::StoppedState);

        if (currentStatus != DragonPlayer::MediaStatus::LoadedMedia) {
            currentStatus = DragonPlayer::MediaStatus::LoadedMedia;
        }
        Q_EMIT q->statusChanged(DragonPlayer::MediaStatus::LoadedMedia);
    }

    void writeToQueues(std::span<const std::float32_t> pcm)
    {
        if (pcm.empty()) {
            return;
        }

        writeToQueueWithBackpressure(*audioQueue, pcm);
    }

    size_t writeToQueueWithBackpressure(LockFreeSpscQueue<std::float32_t> &queue, std::span<const std::float32_t> pcm)
    {
        size_t written = 0;
        while (written < pcm.size() && !decodeStopSource.stop_requested()) {
            auto remaining = pcm.subspan(written);
            size_t n = queue.try_write(remaining.size(), [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
                auto in_iter = std::ranges::copy_n(remaining.begin(), b1.size(), b1.begin()).in;
                std::ranges::copy_n(in_iter, b2.size(), b2.begin());
            });
            written += n;
            if (written < pcm.size() && !decodeStopSource.stop_requested()) {
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
            ensureFftInfrastructure();

            if (audioOutput && fftQueue) {
                audioOutput->setFftQueue(fftQueue.get());
            }

            fftProcessor->setFftMode(mode);

            startFftThread();
        } else if (wasOn && !nowOn) {
            if (audioOutput) {
                audioOutput->setFftQueue(nullptr);
            }
            if (fftProcessor) {
                fftProcessor->setFftMode(mode);
            }
        } else if (nowOn && fftProcessor) {
            fftProcessor->setFftMode(mode);
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

        {
            std::lock_guard lock(decoderMutex);
            if (activeDecoder) {
                activeDecoder->requestSeek(posMs);
            }
        }

        if (audioOutput) {
            audioOutput->setPositionOffset(posMs);
            audioOutput->clearStream();
        }
        Q_EMIT q->positionChanged(posMs);
    }

    DragonPlayer *q;

    std::vector<std::float32_t> fftBuffer;
    std::vector<std::float32_t> audioBuffer;
    std::unique_ptr<LockFreeSpscQueue<std::float32_t>> fftQueue;
    std::unique_ptr<LockFreeSpscQueue<std::float32_t>> audioQueue;

    std::unique_ptr<DragonAudioOutput> audioOutput;
    std::unique_ptr<DragonFftProcessor> fftProcessor;
    std::unique_ptr<DragonRadioStream> radioStream;

    std::jthread decodeThread;
    std::stop_source decodeStopSource;
    std::mutex decoderMutex;
    std::condition_variable decoderCv;
    bool decodeLoopActive = false;
    std::unique_ptr<DragonDecoder> activeDecoder;
    std::unique_ptr<DragonDecoder> preWarmedDecoder;

    std::jthread preWarmThread;

    std::jthread fftThread;

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