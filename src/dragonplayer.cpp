/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragonaudiooutput.h>
#include <dragonsdl/dragondecoder.h>
#include <dragonsdl/dragonfftprocessor.h>
#include <dragonsdl/dragonplayer.h>
#include <dragonsdl/dragonradiostream.h>

#include <LockFreeSpscQueue.h>

#include <QDebug>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <ranges>
#include <stdfloat>
#include <stop_token>
#include <thread>
#include <utility>

class DragonPlayer::DragonPlayerPrivate
{
    static constexpr size_t kBufferCapacity = 65536;

public:
    explicit DragonPlayerPrivate(DragonPlayer *player)
        : q(player)
    {
        fftBuffer.resize(kBufferCapacity);
        audioBuffer.resize(kBufferCapacity);
        fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));
        audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(audioBuffer));

        audioOutput = std::make_unique<DragonAudioOutput>();
        audioOutput->setQueue(audioQueue.get());

        QObject::connect(audioOutput.get(), &DragonAudioOutput::errorOccurred, q, [this](const QString &) {
            setError(Error::ResourceError);
        });

        QObject::connect(audioOutput.get(), &DragonAudioOutput::volumeChanged, q, &DragonPlayer::volumeChanged);

        fftProcessor = std::make_unique<DragonFftProcessor>();
        fftProcessor->setQueue(fftQueue.get());

        fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            QMetaObject::invokeMethod(
                q,
                [this, f = std::move(frame)]() mutable {
                    Q_EMIT q->fftFrameReady(f);
                },
                Qt::QueuedConnection);
        });

        positionTimer = new QTimer(q);
        positionTimer->setInterval(100);
        QObject::connect(positionTimer, &QTimer::timeout, q, [this]() {
            Q_EMIT q->positionChanged(position());
        });

        startThreads();
    }

    ~DragonPlayerPrivate()
    {
        stopPipeline();
    }

    void startThreads()
    {
        fftThread = std::jthread([this](std::stop_token st) {
            fftProcessor->processLoop(st);
        });

        decodeThread = std::jthread([this](std::stop_token st) {
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
                            setError(currentIsLocal ? Error::FormatError : Error::NetworkError);
                            setStatus(MediaStatus::InvalidMedia);
                            setPlaybackState(PlaybackState::StoppedState);
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
                            setStatus(MediaStatus::EndOfMedia);
                            setPlaybackState(PlaybackState::StoppedState);
                        },
                        Qt::QueuedConnection);
                }
            }
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

            QObject::connect(radioStream.get(), &DragonRadioStream::errorOccurred, q, [this](const QString &) {
                setError(Error::NetworkError);
            });

            QObject::connect(radioStream.get(), &DragonRadioStream::metadataReady, q, [this](const QString &title, const QString &artistOrStation) {
                Q_EMIT q->currentPlayingForRadiosChanged(title, artistOrStation);
            });

            radioStream->start();
        } else if (radioStream) {
            radioStream->stop();
            radioStream.reset();
        }

        DragonDecoder::ReadCallback readCb;
        if (!isLocal) {
            readCb = [this](std::span<uint8_t> buf) -> int {
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
                    qDebug() << "PLAYER: ignoring stale formatReady (gen" << generation << "!= current" << currentDecoderGeneration << ")";
                    return;
                }
                qDebug() << "PLAYER: formatReady sr=" << sampleRate << "ch=" << channels;
                if (!isGapless || !audioOutput->hasFormat(sampleRate, channels)) {
                    audioOutput->start(sampleRate, channels);
                }
                audioOutput->setPositionOffset(currentPosition);
                fftProcessor->setSampleRate(sampleRate);
                setStatus(MediaStatus::LoadedMedia);
            },
            Qt::AutoConnection);

        connect(
            decoder.get(),
            &DragonDecoder::durationChanged,
            q,
            [this](int64_t durationMs) {
                QMetaObject::invokeMethod(
                    q,
                    [this, durationMs]() {
                        currentDuration = durationMs;
                        Q_EMIT q->durationChanged(durationMs);
                    },
                    Qt::QueuedConnection);
            },
            Qt::DirectConnection);

        connect(
            decoder.get(),
            &DragonDecoder::streamError,
            q,
            [this](const QString &msg) {
                qDebug() << "Decoder error:" << msg;
                QMetaObject::invokeMethod(
                    q,
                    [this]() {
                        setError(currentIsLocal ? Error::FormatError : Error::NetworkError);
                    },
                    Qt::QueuedConnection);
            },
            Qt::DirectConnection);

        return decoder;
    }

    void setSource(const QUrl &source)
    {
        qDebug() << "PLAYER: setSource(" << source.toString() << ")";

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

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }
        audioQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(audioBuffer));
        fftQueue = std::make_unique<LockFreeSpscQueue<std::float32_t>>(std::span(fftBuffer));
        audioOutput->setQueue(audioQueue.get());
        fftProcessor->setQueue(fftQueue.get());
        fftProcessor->reset();

        fftThread = std::jthread([this](std::stop_token st) {
            fftProcessor->processLoop(st);
        });

        currentSource = source;
        currentPosition = 0;
        currentDuration = 0;
        nextSource.clear();

        Q_EMIT q->sourceChanged();
        Q_EMIT q->nextSourceChanged();

        if (source.isEmpty()) {
            setStatus(MediaStatus::NoMedia);
            setPlaybackState(PlaybackState::StoppedState);
            return;
        }

        setError(Error::NoError);
        setStatus(MediaStatus::LoadingMedia);

        const bool isLocal = source.isLocalFile();
        currentIsLocal = isLocal;
        currentSeekable = isLocal;
        Q_EMIT q->seekableChanged(currentSeekable);

        auto decoder = createDecoder(source, false, currentDecoderGeneration);
        if (!decoder) {
            setError(Error::FormatError);
            setStatus(MediaStatus::InvalidMedia);
            return;
        }

        {
            std::lock_guard lock(decoderMutex);
            activeDecoder = std::move(decoder);
        }

        decodeStopSource = std::stop_source{};
        decoderCv.notify_one();

        setPlaybackState(PlaybackState::PlayingState);
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
        qDebug() << "PLAYER: stopPipeline() full teardown";

        if (decodeStopSource.stop_possible()) {
            decodeStopSource.request_stop();
        }
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

        qDebug() << "PLAYER: stopPipeline() teardown complete";
    }

    void play()
    {
        if (currentSource.isEmpty()) {
            return;
        }

        if (currentPlaybackState == PlaybackState::PausedState) {
            if (audioOutput) {
                audioOutput->resume();
            }
            setPlaybackState(PlaybackState::PlayingState);
            return;
        }

        if (currentStatus == MediaStatus::EndOfMedia || currentStatus == MediaStatus::LoadedMedia) {
            setSource(currentSource);
            return;
        }

        setPlaybackState(PlaybackState::PlayingState);
    }

    void pause()
    {
        if (audioOutput) {
            audioOutput->pause();
        }
        setPlaybackState(PlaybackState::PausedState);
    }

    void stop()
    {
        qDebug() << "PLAYER: stop()";

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

        setPlaybackState(PlaybackState::StoppedState);
    }

    void writeToQueues(std::span<const std::float32_t> pcm)
    {
        if (pcm.empty()) {
            return;
        }

        const size_t pcmSize = pcm.size();

        const size_t audioWritten = writeToQueueWithBackpressure(*audioQueue, pcm);

        const size_t fftFreeBefore = fftQueue->get_num_free();
        const size_t fftWritten = fftQueue->try_write(pcmSize, [&](std::span<std::float32_t> b1, std::span<std::float32_t> b2) {
            auto in_iter = std::ranges::copy_n(pcm.begin(), b1.size(), b1.begin()).in;
            std::ranges::copy_n(in_iter, b2.size(), b2.begin());
        });

        if (fftWritten < pcmSize) {
            qWarning() << "WRITE_Q: FFT QUEUE OVERFLOW dropped" << (pcmSize - fftWritten) << "samples";
        }
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

    bool isAudioActive() const
    {
        return audioOutput ? audioOutput->isDeviceOpen() : false;
    }

    void setPlaybackState(PlaybackState state)
    {
        if (currentPlaybackState == state) {
            return;
        }

        if (state == PlaybackState::PlayingState) {
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
        case PlaybackState::PlayingState:
            Q_EMIT q->playing();
            break;
        case PlaybackState::PausedState:
            Q_EMIT q->paused();
            break;
        case PlaybackState::StoppedState:
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

        if (status == MediaStatus::InvalidMedia) {
            setError(Error::FormatError);
        }
    }

    void setError(DragonPlayer::Error error)
    {
        if (currentError == error) {
            return;
        }
        currentError = error;
        Q_EMIT q->errorChanged(error);

        if (error != Error::NoError) {
            setStatus(MediaStatus::InvalidMedia);
        }
    }

    float volume() const
    {
        return currentVolume;
    }

    void setVolume(float gain)
    {
        currentVolume = gain;
        if (audioOutput) {
            audioOutput->setVolume(gain);
        }
        Q_EMIT q->volumeChanged();
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
    PlaybackState currentPlaybackState = PlaybackState::StoppedState;
    MediaStatus currentStatus = MediaStatus::NoMedia;
    Error currentError = Error::NoError;
    int64_t currentDuration = 0;
    float currentVolume = 1.0f;
    bool currentMuted = false;
    bool currentSeekable = false;
    bool currentIsLocal = false;

    uint64_t currentDecoderGeneration = 0;

    int64_t undoPosition = 0;

    QTimer *positionTimer = nullptr;
    int64_t currentPosition = 0;
};

DragonPlayer::DragonPlayer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<DragonPlayerPrivate>(this))
{
}

DragonPlayer::~DragonPlayer() = default;

bool DragonPlayer::muted() const
{
    return d->muted();
}
float DragonPlayer::volume() const
{
    return d->volume();
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
    return d->position();
}
bool DragonPlayer::seekable() const
{
    return d->currentSeekable;
}
bool DragonPlayer::isAudioActive() const
{
    return d->isAudioActive();
}

void DragonPlayer::setMuted(bool muted)
{
    d->setMuted(muted);
}
void DragonPlayer::setVolume(float gain)
{
    d->setVolume(gain);
}
void DragonPlayer::setSource(const QUrl &source)
{
    d->setSource(source);
}
void DragonPlayer::setNextSource(const QUrl &nextSource)
{
    d->setNextSource(nextSource);
}

void DragonPlayer::setPosition(int64_t posMs)
{
    d->setPosition(posMs);
}

void DragonPlayer::play()
{
    d->play();
}
void DragonPlayer::pause()
{
    d->pause();
}
void DragonPlayer::stop()
{
    d->stop();
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
