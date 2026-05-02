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
    }

    ~DragonPlayerPrivate()
    {
        stopPipeline();
    }

    void setSource(const QUrl &source)
    {
        stopPipeline();

        currentSource = source;
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
            radioStream = std::make_unique<DragonRadioStream>();
            radioStream->setUrl(source);

            QObject::connect(radioStream.get(), &DragonRadioStream::errorOccurred, q, [this](const QString &) {
                setError(Error::NetworkError);
            });

            QObject::connect(radioStream.get(), &DragonRadioStream::metadataReady, q, [this](const QString &title, const QString &artistOrStation) {
                Q_EMIT q->currentPlayingForRadiosChanged(title, artistOrStation);
            });

            radioStream->start();
        }

        decodeStopSource = std::stop_source{};
        fftStopSource = std::stop_source{};

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
            [this](int sampleRate, int channels) {
                audioOutput->start(sampleRate, channels);
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

        fftProcessor->reset();

        decodeThread = std::jthread([this](std::stop_token st) {
            decoder->decodeLoop(st);

            QMetaObject::invokeMethod(
                q,
                [this]() {
                    setStatus(MediaStatus::EndOfMedia);
                    setPlaybackState(PlaybackState::StoppedState);
                },
                Qt::QueuedConnection);
        });

        fftThread = std::jthread([this](std::stop_token st) {
            fftProcessor->processLoop(st);
        });

        setPlaybackState(PlaybackState::PlayingState);
    }

    void stopPipeline()
    {
        if (decodeStopSource.stop_possible()) {
            decodeStopSource.request_stop();
        }
        if (fftStopSource.stop_possible()) {
            fftStopSource.request_stop();
        }

        decodeThread = std::jthread{};
        fftThread = std::jthread{};

        if (audioOutput) {
            audioOutput->stop();
            audioOutput->reset();
        }

        decoder.reset();

        if (radioStream) {
            radioStream->stop();
            radioStream.reset();
        }
    }

    void play()
    {
        if (currentSource.isEmpty()) {
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
        setPlaybackState(PlaybackState::PausedState);
    }

    void stop()
    {
        setPlaybackState(PlaybackState::StoppedState);
    }

    void writeToQueues(std::span<const float> pcm)
    {
        if (pcm.empty()) {
            return;
        }

        [[maybe_unused]] auto written = audioQueue->try_write(pcm.size(), [&](std::span<float> b1, std::span<float> b2) {
            size_t i = 0;
            for (auto &v : b1) {
                v = pcm[i++];
            }
            for (auto &v : b2) {
                v = pcm[i++];
            }
        });

        [[maybe_unused]] auto fftWritten = fftQueue->try_write(pcm.size(), [&](std::span<float> b1, std::span<float> b2) {
            size_t i = 0;
            for (auto &v : b1) {
                v = pcm[i++];
            }
            for (auto &v : b2) {
                v = pcm[i++];
            }
        });
    }

    void setPlaybackState(PlaybackState state)
    {
        if (currentPlaybackState == state) {
            return;
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
        return audioOutput ? audioOutput->positionMs() : 0;
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
    PlaybackState currentPlaybackState = PlaybackState::StoppedState;
    MediaStatus currentStatus = MediaStatus::NoMedia;
    Error currentError = Error::NoError;
    int64_t currentDuration = 0;
    float currentVolume = 1.0f;
    bool currentMuted = false;
    bool currentSeekable = false;
    bool currentIsLocal = false;

    int64_t undoPosition = 0;
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

void DragonPlayer::setPosition(int64_t posMs)
{
    Q_UNUSED(posMs);
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