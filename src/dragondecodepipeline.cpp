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

DragonDecodePipeline::DragonDecodePipeline(DragonPlayer *player)
    : q(player)
{
    startDecodeThread();
}

DragonDecodePipeline::~DragonDecodePipeline()
{
    stop();
}

void DragonDecodePipeline::startDecodeThread()
{
    m_decodeThread = std::jthread([this](std::stop_token st) {
        setCurrentThreadName("dragon-decode");
        while (!st.stop_requested()) {
            std::unique_lock lock(m_decoderMutex);
            m_decoderCv.wait(lock, [this, &st]() {
                return m_activeDecoder != nullptr || st.stop_requested();
            });
            if (st.stop_requested())
                break;

            DragonDecoder *decoder = m_activeDecoder.get();
            m_decodeLoopActive = true;
            lock.unlock();

            decoder->decodeLoop(m_decodeStopSource.get_token());
            const bool hadFatalError = decoder->hasFatalError();

            lock.lock();
            m_decodeLoopActive = false;
            const bool wasStopped = m_decodeStopSource.stop_requested();

            m_activeDecoder.reset();

            lock.unlock();
            m_decoderCv.notify_all();

            if (st.stop_requested())
                break;

            if (wasStopped) {
                continue;
            }

            if (hadFatalError) {
                if (m_finishedCallback) {
                    m_finishedCallback(true, false);
                }
                continue;
            }

            std::unique_lock plock(m_decoderMutex);
            if (m_preWarmedDecoder) {
                m_activeDecoder = std::move(m_preWarmedDecoder);
                QUrl newSource = m_nextSource;
                m_nextSource.clear();

                plock.unlock();

                if (m_gaplessTransitionCallback) {
                    m_gaplessTransitionCallback(newSource);
                }
            } else {
                plock.unlock();

                if (m_finishedCallback) {
                    m_finishedCallback(false, wasStopped);
                }
            }
        }
    });
}

void DragonDecodePipeline::stopSession()
{
    m_decodeStopSource.request_stop();

    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
    }

    {
        std::unique_lock lock(m_decoderMutex);
        m_decoderCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        m_activeDecoder.reset();
    }

    if (m_preWarmThread.joinable()) {
        m_preWarmThread.join();
    }
}

void DragonDecodePipeline::stop()
{
    m_decodeStopSource.request_stop();
    m_decodeThread.request_stop();

    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
    }

    m_decoderCv.notify_all();

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.join();
    }

    {
        std::lock_guard lock(m_decoderMutex);
        m_activeDecoder.reset();
        m_preWarmedDecoder.reset();
        m_decodeLoopActive = false;
    }

    if (m_radioStream) {
        m_radioStream->stop();
        m_radioStream.reset();
    }
}

void DragonDecodePipeline::setSource(const QUrl &source, uint64_t generation)
{
    m_generation = generation;

    if (!m_decodeThread.joinable()) {
        startDecodeThread();
    }

    {
        std::lock_guard lock(m_decoderMutex);
        m_preWarmedDecoder.reset();
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }

    m_decodeStopSource.request_stop();

    {
        std::unique_lock lock(m_decoderMutex);
        m_decoderCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        m_activeDecoder.reset();
    }

    auto decoder = createDecoder(source, false, generation);
    if (!decoder) {
        return;
    }

    {
        std::lock_guard lock(m_decoderMutex);
        m_activeDecoder = std::move(decoder);
    }

    m_decodeStopSource = std::stop_source{};
    m_decoderCv.notify_one();
}

void DragonDecodePipeline::setNextSource(const QUrl &next, uint64_t generation)
{
    m_nextSource = next;

    {
        std::lock_guard lock(m_decoderMutex);
        m_preWarmedDecoder.reset();
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }

    if (next.isEmpty() || !next.isLocalFile()) {
        return;
    }

    m_preWarmThread = std::jthread([this, next, generation](std::stop_token st) {
        setCurrentThreadName("dragon-prewarm");
        auto decoder = createDecoder(next, true, generation);
        if (st.stop_requested()) {
            return;
        }
        if (decoder) {
            std::lock_guard lock(m_decoderMutex);
            if (!st.stop_requested()) {
                m_preWarmedDecoder = std::move(decoder);
            }
        }
    });
}

bool DragonDecodePipeline::isActive() const
{
    std::lock_guard lock(m_decoderMutex);
    return m_activeDecoder != nullptr;
}

bool DragonDecodePipeline::hasFatalError() const
{
    std::lock_guard lock(m_decoderMutex);
    return m_activeDecoder && m_activeDecoder->hasFatalError();
}

void DragonDecodePipeline::requestSeek(int64_t posMs)
{
    std::lock_guard lock(m_decoderMutex);
    if (m_activeDecoder) {
        m_activeDecoder->requestSeek(posMs);
    }
}

std::unique_ptr<DragonDecoder> DragonDecodePipeline::createDecoder(const QUrl &source, bool isGapless, uint64_t generation)
{
    const bool isLocal = source.isLocalFile();

    if (!isLocal) {
        if (m_radioStream) {
            m_radioStream->stop();
            m_radioStream.reset();
        }
        m_radioStream = std::make_unique<DragonRadioStream>();
        m_radioStream->setUrl(source);

        QObject::connect(m_radioStream.get(), &DragonRadioStream::errorOccurred, q, [this](const QString &) {
            if (m_errorCallback) {
                m_errorCallback(QStringLiteral("Network error"));
            }
        });

        QObject::connect(m_radioStream.get(), &DragonRadioStream::metadataReady, q, [this](const DragonIcyMetadata &metadata) {
            Q_EMIT q->currentPlayingForRadiosChanged(metadata);
        });

        m_radioStream->start();
    } else if (m_radioStream) {
        m_radioStream->stop();
        m_radioStream.reset();
    }

    DragonDecoder::ReadCallback readCb;
    if (!isLocal) {
        readCb = [this](const std::span<uint8_t> buf) -> int {
            return m_radioStream ? m_radioStream->read(buf, m_decodeStopSource.get_token()) : -1;
        };
    }

    auto decoder = std::make_unique<DragonDecoder>(std::move(readCb), isLocal ? source.toLocalFile() : QString{});

    decoder->setSamplesCallback([this](std::span<const std::float32_t> data, int, int) {
        if (m_samplesCallback) {
            m_samplesCallback(data, m_decodeStopSource.get_token());
        }
    });

    QObject::connect(
        decoder.get(),
        &DragonDecoder::formatReady,
        q,
        [this, generation, isGapless](int sampleRate, int channels) {
            if (m_generation != generation) {
                qCDebug(dragonsdlPlayer) << "ignoring stale formatReady (gen" << generation << "!= current" << m_generation << ")";
                return;
            }
            qCDebug(dragonsdlPlayer) << "formatReady sr=" << sampleRate << "ch=" << channels;
            if (m_formatReadyCallback) {
                m_formatReadyCallback(sampleRate, channels, isGapless);
            }
        },
        Qt::QueuedConnection);

    QObject::connect(
        decoder.get(),
        &DragonDecoder::durationChanged,
        q,
        [this](int64_t durationMs) {
            if (m_durationCallback) {
                m_durationCallback(durationMs);
            }
        },
        Qt::QueuedConnection);

    QObject::connect(
        decoder.get(),
        &DragonDecoder::streamError,
        q,
        [this](const QString &msg) {
            qCDebug(dragonsdlPlayer) << "Decoder error:" << msg;
            if (m_errorCallback) {
                m_errorCallback(msg);
            }
        },
        Qt::QueuedConnection);

    return decoder;
}

uint64_t DragonDecodePipeline::generation() const
{
    return m_generation;
}

void DragonDecodePipeline::setCallbacks(FormatReadyCallback format,
                                        DurationCallback duration,
                                        SamplesCallback samples,
                                        ErrorCallback error,
                                        FinishedCallback finished,
                                        GaplessTransitionCallback gapless)
{
    m_formatReadyCallback = std::move(format);
    m_durationCallback = std::move(duration);
    m_samplesCallback = std::move(samples);
    m_errorCallback = std::move(error);
    m_finishedCallback = std::move(finished);
    m_gaplessTransitionCallback = std::move(gapless);
}

const std::unique_ptr<DragonDecoder> &DragonDecodePipeline::activeDecoder() const
{
    return m_activeDecoder;
}

bool DragonDecodePipeline::decodeLoopActive() const
{
    return m_decodeLoopActive;
}
