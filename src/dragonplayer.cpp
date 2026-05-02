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
#include <stop_token>
#include <thread>

class DragonPlayer::DragonPlayerPrivate
{
public:
    explicit DragonPlayerPrivate(DragonPlayer *player)
        : q(player)
    {
        fftBuffer.resize(65536);
        audioBuffer.resize(65536);
        fftQueue = std::make_unique<LockFreeSpscQueue<float>>(std::span(fftBuffer));
        audioQueue = std::make_unique<LockFreeSpscQueue<float>>(std::span(audioBuffer));

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
    }

    ~DragonPlayerPrivate()
    {
        stopPipeline();
    }

    void setSource(const QUrl &source)
    {
        qDebug() << "PLAYER: setSource(" << source.toString() << ")";
        stopPipeline();
        startTrack(source, false);
    }

    void setNextSource(const QUrl &next)
    {
        nextSource = next;
        Q_EMIT q->nextSourceChanged();
    }

    void startTrack(const QUrl &source, bool seamless)
    {
        if (!seamless) {
            audioQueue = std::make_unique<LockFreeSpscQueue<float>>(std::span(audioBuffer));
            fftQueue = std::make_unique<LockFreeSpscQueue<float>>(std::span(fftBuffer));
            audioOutput->setQueue(audioQueue.get());
            fftProcessor->setQueue(fftQueue.get());
            qDebug() << "PLAYER: queues recreated audioQueue cap=" << audioQueue->get_capacity() << "free=" << audioQueue->get_num_free()
                     << "ready=" << audioQueue->get_num_items_ready() << "fftQueue cap=" << fftQueue->get_capacity() << "free=" << fftQueue->get_num_free()
                     << "ready=" << fftQueue->get_num_items_ready();
        }

        currentSource = source;
        currentPosition = 0;
        Q_EMIT q->sourceChanged();

        if (source.isEmpty()) {
            setStatus(MediaStatus::NoMedia);
            return;
        }

        setError(Error::NoError);
        setStatus(MediaStatus::LoadingMedia);

        const bool isLocal = source.isLocalFile();
        currentIsLocal = isLocal;
        currentSeekable = isLocal;
        Q_EMIT q->seekableChanged(currentSeekable);

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

        decodeStopSource = std::stop_source{};
        if (!seamless) {
            fftStopSource = std::stop_source{};
        }

        DragonDecoder::ReadCallback readCb;
        if (!isLocal) {
            readCb = [this](std::span<uint8_t> buf) -> int {
                return radioStream ? radioStream->read(buf, decodeStopSource.get_token()) : -1;
            };
        }

        decoder = std::make_unique<DragonDecoder>(std::move(readCb), isLocal ? source.toLocalFile() : QString{});

        QObject::connect(
            decoder.get(),
            &DragonDecoder::formatReady,
            q,
            [this, seamless](int sampleRate, int channels) {
                qDebug() << "PLAYER: formatReady sr=" << sampleRate << "ch=" << channels;
                if (!seamless || !audioOutput->hasFormat(sampleRate, channels)) {
                    audioOutput->start(sampleRate, channels);
                }
                audioOutput->setPositionOffset(currentPosition);
                fftProcessor->setSampleRate(sampleRate);
                setStatus(MediaStatus::LoadedMedia);
            },
            Qt::AutoConnection);

        QObject::connect(
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

        QObject::connect(
            decoder.get(),
            &DragonDecoder::samplesDecoded,
            q,
            [this](std::span<const float> data, int, int) {
                writeToQueues(data);
            },
            Qt::DirectConnection);

        QObject::connect(
            decoder.get(),
            &DragonDecoder::streamError,
            q,
            [this](const QString &msg) {
                qWarning() << "Decoder error:" << msg;

                QMetaObject::invokeMethod(
                    q,
                    [this]() {
                        setError(currentIsLocal ? Error::FormatError : Error::NetworkError);
                    },
                    Qt::QueuedConnection);
            },
            Qt::DirectConnection);

        if (!seamless) {
            fftProcessor->reset();
        }

        decodeThread = std::jthread([this](std::stop_token st) {
            decoder->decodeLoop(st);

            if (!st.stop_requested()) {
                QMetaObject::invokeMethod(
                    q,
                    [this]() {
                        if (!nextSource.isEmpty()) {
                            QUrl url = nextSource;
                            nextSource.clear();
                            Q_EMIT q->nextSourceChanged();
                            transitionToNextTrack(url);
                        } else {
                            setStatus(MediaStatus::EndOfMedia);
                            setPlaybackState(PlaybackState::StoppedState);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });

        if (!seamless) {
            fftThread = std::jthread([this](std::stop_token st) {
                fftProcessor->processLoop(st);
            });
        }

        setPlaybackState(PlaybackState::PlayingState);
    }

    void stopPipeline()
    {
        qDebug() << "PLAYER: stopPipeline() begin teardown";

        if (decodeStopSource.stop_possible()) {
            qDebug() << "PLAYER: requesting decode stop";
            decodeStopSource.request_stop();
        }
        if (fftStopSource.stop_possible()) {
            qDebug() << "PLAYER: requesting FFT stop";
            fftStopSource.request_stop();
        }

        qDebug() << "PLAYER: joining decode thread...";
        decodeThread = std::jthread{};
        qDebug() << "PLAYER: joining FFT thread...";
        fftThread = std::jthread{};

        if (audioOutput) {
            qDebug() << "PLAYER: stopping audio output...";
            audioOutput->stop();
            audioOutput->reset();
        }

        qDebug() << "PLAYER: destroying decoder...";
        decoder.reset();

        if (radioStream) {
            qDebug() << "PLAYER: stopping radio stream...";
            radioStream->stop();
            radioStream.reset();
        }

        qDebug() << "PLAYER: stopPipeline() teardown complete";
    }

    void stopDecodeOnly()
    {
        qDebug() << "PLAYER: stopDecodeOnly() stopping decode side only";

        if (decodeStopSource.stop_possible()) {
            decodeStopSource.request_stop();
        }
        decodeThread = std::jthread{};
        decoder.reset();

        qDebug() << "PLAYER: stopDecodeOnly() decode side stopped";
    }

    void transitionToNextTrack(const QUrl &source)
    {
        qDebug() << "PLAYER: transitionToNextTrack(" << source.toString() << ")";
        stopDecodeOnly();
        startTrack(source, true);
        Q_EMIT q->trackChanged();
        qDebug() << "PLAYER: seamless transition complete";
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
        stopPipeline();
        setPlaybackState(PlaybackState::StoppedState);
    }

    void writeToQueues(std::span<const float> pcm)
    {
        if (pcm.empty()) {
            return;
        }

        const size_t pcmSize = pcm.size();

        const size_t audioWritten = writeToQueueWithBackpressure(*audioQueue, pcm);

        const size_t fftFreeBefore = fftQueue->get_num_free();
        const size_t fftWritten = fftQueue->try_write(pcmSize, [&](std::span<float> b1, std::span<float> b2) {
            size_t i = 0;
            for (auto &v : b1)
                v = pcm[i++];
            for (auto &v : b2)
                v = pcm[i++];
        });

        if (fftWritten < pcmSize) {
            qWarning() << "WRITE_Q: FFT QUEUE OVERFLOW dropped" << (pcmSize - fftWritten) << "samples";
        }
    }

    size_t writeToQueueWithBackpressure(LockFreeSpscQueue<float> &queue, std::span<const float> pcm)
    {
        size_t written = 0;
        while (written < pcm.size() && !decodeStopSource.stop_requested()) {
            auto remaining = pcm.subspan(written);
            size_t n = queue.try_write(remaining.size(), [&](std::span<float> b1, std::span<float> b2) {
                size_t i = 0;
                for (auto &v : b1)
                    v = remaining[i++];
                for (auto &v : b2)
                    v = remaining[i++];
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
        posMs = std::max(posMs, int64_t{0});
        if (currentDuration > 0) {
            posMs = std::min(posMs, currentDuration);
        }
        currentPosition = posMs;
        if (decoder) {
            decoder->requestSeek(posMs);
        }
        if (audioOutput) {
            audioOutput->setPositionOffset(posMs);
        }
        Q_EMIT q->positionChanged(posMs);
    }

    DragonPlayer *q;

    std::vector<float> fftBuffer;
    std::vector<float> audioBuffer;
    std::unique_ptr<LockFreeSpscQueue<float>> fftQueue;
    std::unique_ptr<LockFreeSpscQueue<float>> audioQueue;

    std::unique_ptr<DragonAudioOutput> audioOutput;
    std::unique_ptr<DragonFftProcessor> fftProcessor;
    std::unique_ptr<DragonDecoder> decoder;
    std::unique_ptr<DragonRadioStream> radioStream;

    std::jthread decodeThread;
    std::jthread fftThread;
    std::stop_source decodeStopSource;
    std::stop_source fftStopSource;

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
