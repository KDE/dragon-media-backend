/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragondecodepipeline.h"

#include "dragondecoder.h"
#include "dragonradiostream.h"
#include <dragonsdl/dragonicymetadata.h>
#include <dragonsdl/dragonplayer.h>

#include "dragonsdl_logging.h"

#include <QMetaObject>
#include <QObject>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>

static inline void setCurrentThreadName(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

class DragonDecodePipeline::Impl
{
public:
    explicit Impl(DragonPlayer *player)
        : q(player)
    {
        startDecodeThread();
    }

    ~Impl()
    {
        stop();
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
                    if (finishedCallback) {
                        finishedCallback(true, false);
                    }
                    continue;
                }

                std::unique_lock plock(decoderMutex);
                if (preWarmedDecoder) {
                    activeDecoder = std::move(preWarmedDecoder);
                    QUrl newSource = nextSource;
                    nextSource.clear();

                    plock.unlock();

                    if (gaplessTransitionCallback) {
                        gaplessTransitionCallback(newSource);
                    }
                } else {
                    plock.unlock();

                    if (finishedCallback) {
                        finishedCallback(false, wasStopped);
                    }
                }
            }
        });
    }

    void stop()
    {
        decodeStopSource.request_stop();
        decodeThread.request_stop();

        if (preWarmThread.joinable()) {
            preWarmThread.request_stop();
        }

        decoderCv.notify_all();

        if (decodeThread.joinable()) {
            decodeThread.join();
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

        if (radioStream) {
            radioStream->stop();
            radioStream.reset();
        }
    }

    void setSource(const QUrl &source, uint64_t generation)
    {
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

        auto decoder = createDecoder(source, false, generation);
        if (!decoder) {
            return;
        }

        {
            std::lock_guard lock(decoderMutex);
            activeDecoder = std::move(decoder);
        }

        decodeStopSource = std::stop_source{};
        decoderCv.notify_one();
    }

    void setNextSource(const QUrl &next, uint64_t generation)
    {
        nextSource = next;

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

        preWarmThread = std::jthread([this, next, generation](std::stop_token st) {
            setCurrentThreadName("dragon-prewarm");
            auto decoder = createDecoder(next, true, generation);
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

    bool isActive() const
    {
        std::lock_guard lock(decoderMutex);
        return activeDecoder != nullptr;
    }

    bool hasFatalError() const
    {
        std::lock_guard lock(decoderMutex);
        return activeDecoder && activeDecoder->hasFatalError();
    }

    void requestSeek(int64_t posMs)
    {
        std::lock_guard lock(decoderMutex);
        if (activeDecoder) {
            activeDecoder->requestSeek(posMs);
        }
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
                if (errorCallback) {
                    errorCallback(QStringLiteral("Network error"));
                }
            });

            QObject::connect(radioStream.get(), &DragonRadioStream::metadataReady, q, [this](const DragonIcyMetadata &metadata) {
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
            if (samplesCallback) {
                samplesCallback(data, decodeStopSource.get_token());
            }
        });

        QObject::connect(
            decoder.get(),
            &DragonDecoder::formatReady,
            q,
            [this, generation, isGapless](int sampleRate, int channels) {
                if (this->generation != generation) {
                    qCDebug(dragonsdlPlayer) << "ignoring stale formatReady (gen" << generation << "!= current" << this->generation << ")";
                    return;
                }
                qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels;
                if (formatReadyCallback) {
                    formatReadyCallback(sampleRate, channels, isGapless);
                }
            },
            Qt::QueuedConnection);

        QObject::connect(
            decoder.get(),
            &DragonDecoder::durationChanged,
            q,
            [this](int64_t durationMs) {
                if (durationCallback) {
                    durationCallback(durationMs);
                }
            },
            Qt::QueuedConnection);

        QObject::connect(
            decoder.get(),
            &DragonDecoder::streamError,
            q,
            [this](const QString &msg) {
                qCDebug(dragonsdlPlayer) << "Decoder error:" << msg;
                if (errorCallback) {
                    errorCallback(msg);
                }
            },
            Qt::QueuedConnection);

        return decoder;
    }

    const std::unique_ptr<DragonDecoder> &getActiveDecoder() const
    {
        return activeDecoder;
    }
    bool isDecodeLoopActive() const
    {
        return decodeLoopActive;
    }

    FormatReadyCallback formatReadyCallback;
    DurationCallback durationCallback;
    SamplesCallback samplesCallback;
    ErrorCallback errorCallback;
    FinishedCallback finishedCallback;
    GaplessTransitionCallback gaplessTransitionCallback;

    DragonPlayer *q = nullptr;
    uint64_t generation = 0;
    QUrl nextSource;

    std::jthread decodeThread;
    std::stop_source decodeStopSource;
    mutable std::mutex decoderMutex;
    std::condition_variable decoderCv;
    bool decodeLoopActive = false;
    std::unique_ptr<DragonDecoder> activeDecoder;
    std::unique_ptr<DragonDecoder> preWarmedDecoder;

    std::jthread preWarmThread;

    std::unique_ptr<DragonRadioStream> radioStream;
};

DragonDecodePipeline::DragonDecodePipeline(DragonPlayer *player)
    : d(std::make_unique<Impl>(player))
{
}

DragonDecodePipeline::~DragonDecodePipeline() = default;

void DragonDecodePipeline::setSource(const QUrl &source, uint64_t generation)
{
    d->generation = generation;
    d->setSource(source, generation);
}

void DragonDecodePipeline::setNextSource(const QUrl &next, uint64_t generation)
{
    d->setNextSource(next, generation);
}

void DragonDecodePipeline::stop()
{
    d->stop();
}

void DragonDecodePipeline::requestSeek(int64_t posMs)
{
    d->requestSeek(posMs);
}

bool DragonDecodePipeline::isActive() const
{
    return d->isActive();
}

bool DragonDecodePipeline::hasFatalError() const
{
    return d->hasFatalError();
}

uint64_t DragonDecodePipeline::generation() const
{
    return d->generation;
}

void DragonDecodePipeline::setCallbacks(FormatReadyCallback format,
                                        DurationCallback duration,
                                        SamplesCallback samples,
                                        ErrorCallback error,
                                        FinishedCallback finished,
                                        GaplessTransitionCallback gapless)
{
    d->formatReadyCallback = std::move(format);
    d->durationCallback = std::move(duration);
    d->samplesCallback = std::move(samples);
    d->errorCallback = std::move(error);
    d->finishedCallback = std::move(finished);
    d->gaplessTransitionCallback = std::move(gapless);
}

const std::unique_ptr<DragonDecoder> &DragonDecodePipeline::activeDecoder() const
{
    return d->getActiveDecoder();
}

bool DragonDecodePipeline::decodeLoopActive() const
{
    return d->isDecodeLoopActive();
}
