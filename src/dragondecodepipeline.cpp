/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragondecodepipeline.h"

#include "dragondecoder.h"
#include "dragonradiostream.h"
#include <dragonsdl/dragonicymetadata.h>
#include <dragonsdl/dragonplayer.h>

#include "dragonsdl_decode_logging.h"
#include "dragonsdl_logging.h"

#include <QMetaObject>
#include <QObject>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>

using namespace DragonSdl;

DragonDecodePipeline::DragonDecodePipeline(DragonPlayer *player)
    : QObject(player)
    , m_player(player)
{
    startDecodeThread();
}

void DragonDecodePipeline::startDecodeThread()
{
    qCDebug(dragonsdlDecode) << "startDecodeThread() creating new decode thread";
    m_decodeThread = std::jthread([this](std::stop_token st) {
        pthread_setname_np(pthread_self(), "dragon-decode");
        qCDebug(dragonsdlDecode) << "decode thread started";

        while (!st.stop_requested()) {
            if (!waitForDecoderAssignment(st)) {
                break;
            }

            uint64_t sessionGeneration;
            {
                std::scoped_lock lock(m_decoderMutex);
                sessionGeneration = m_generation;
            }

            auto [wasStopped, hadFatalError] = executeDecodeSession(sessionGeneration);
            if (wasStopped) {
                qCDebug(dragonsdlDecode) << "decode thread session was stopped, continue to wait for new decoder";
                continue;
            }

            if (hadFatalError) {
                qCDebug(dragonsdlDecode) << "decode thread fatal error, emitting finished signal";
                Q_EMIT finished(true);
                Q_EMIT sessionFinished(sessionGeneration, true);
                continue;
            }

            if (st.stop_requested()) {
                qCDebug(dragonsdlDecode) << "decode thread outer stop after loop, breaking";
                break;
            }

            processDecodeCompletion();
        }

        qCDebug(dragonsdlDecode) << "decode thread exiting outer loop";
    });
}

DragonDecodePipeline::~DragonDecodePipeline()
{
    stop();
}

bool DragonDecodePipeline::waitForDecoderAssignment(std::stop_token st)
{
    std::unique_lock lock(m_decoderMutex);
    qCDebug(dragonsdlDecode) << "decode thread waiting for decoder... activeDecoder=" << (m_activeDecoder != nullptr);

    m_decoderAssignedCv.wait(lock, st, [this]() {
        return m_activeDecoder != nullptr;
    });

    qCDebug(dragonsdlDecode) << "decode thread woke up activeDecoder=" << (m_activeDecoder != nullptr) << " outerStopRequested=" << st.stop_requested();

    if (st.stop_requested()) {
        qCDebug(dragonsdlDecode) << "decode thread outer loop stop requested, breaking";
        return false;
    }

    return true;
}

std::pair<bool, bool> DragonDecodePipeline::executeDecodeSession(uint64_t generation)
{
    DragonDecoder *decoder = nullptr;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_decodeLoopActive = true;
        decoder = m_activeDecoder.get();
        if (!decoder) {
            m_decodeLoopActive = false;
        }
    }

    if (!decoder) {
        qCDebug(dragonsdlDecode) << "decode thread decoder became null before decodeLoop, treating as stopped";
        m_decodeLoopFinishedCv.notify_all();
        return {true, false};
    }

    qCDebug(dragonsdlDecode) << "decode thread starting decodeLoop for decoder" << decoder;

    bool initCompleted = false;
    bool hadFatalError = false;
    bool isGapless = false;
    int sessionSampleRate = 0;
    int sessionChannels = 0;

    for (auto event : decoder->decodeLoop(m_sessionStopSource.get_token())) {
        if (m_sessionStopSource.get_token().stop_requested()) {
            qCDebug(dragonsdlDecode) << "decode thread stop requested during iteration";
            break;
        }

        std::visit(overloaded{[&](const FormatReady &fr) {
                                  if (!initCompleted) {
                                      initCompleted = true;
                                      sessionSampleRate = fr.sampleRate;
                                      sessionChannels = fr.channels;

                                      qCDebug(dragonsdlDecode)
                                          << "decode thread FormatReady, emitting formatReady sr=" << fr.sampleRate << "ch=" << fr.channels;
                                      Q_EMIT durationChanged(fr.durationMs);
                                      Q_EMIT formatReady(fr.sampleRate, fr.channels, isGapless);

                                      InitResult result;
                                      result.success = true;
                                      result.sampleRate = fr.sampleRate;
                                      result.channels = fr.channels;
                                      result.durationMs = fr.durationMs;
                                      result.isGapless = isGapless;
                                      result.errorMessage = QString();

                                      Q_EMIT sessionInitCompleted(generation, result);
                                  }
                              },

                              [&](const SamplesChunk &sc) {
                                  if (m_samplesCallback && !sc.data.empty()) {
                                      m_samplesCallback(sc.data, m_sessionStopSource.get_token());
                                  }
                              },

                              [&](const DecodeError &err) {
                                  qCDebug(dragonsdlDecode) << "decode thread DecodeError:" << err.message;

                                  if (!initCompleted) {
                                      initCompleted = true;
                                      hadFatalError = true;

                                      Q_EMIT errorOccurred(err.message);

                                      InitResult result;
                                      result.success = false;
                                      result.errorMessage = err.message;

                                      Q_EMIT sessionInitCompleted(generation, result);
                                  } else {
                                      Q_EMIT errorOccurred(err.message);
                                      Q_EMIT sessionError(generation, err.message);
                                      hadFatalError = true;
                                  }
                              },

                              [&](const DecodeEof &) {
                                  qCDebug(dragonsdlDecode) << "decode thread DecodeEof received";
                              }},
                   event);
    }

    if (!initCompleted && !hadFatalError) {
        if (decoder->hasFatalError()) {
            Q_EMIT errorOccurred(QStringLiteral("Decoder terminated unexpectedly"));

            InitResult result;
            result.success = false;
            result.errorMessage = QStringLiteral("Decoder terminated unexpectedly");

            Q_EMIT sessionInitCompleted(generation, result);
            hadFatalError = true;
        }
    }

    qCDebug(dragonsdlDecode) << "decode thread decodeLoop finished";
    const bool decoderHadFatalError = decoder->hasFatalError();
    bool wasStopped;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_decodeLoopActive = false;

        wasStopped = m_sessionStopSource.stop_requested();
        qCDebug(dragonsdlDecode) << "decode thread loopInactive, wasStopped=" << wasStopped << " hadFatalError=" << hadFatalError;

        m_activeDecoder.reset();
    }
    m_decodeLoopFinishedCv.notify_all();

    return {wasStopped, hadFatalError || decoderHadFatalError};
}

void DragonDecodePipeline::processDecodeCompletion()
{
    uint64_t sessionGeneration = m_generation;

    std::unique_lock plock(m_decoderMutex);
    if (m_preWarmedDecoder) {
        m_activeDecoder = std::move(m_preWarmedDecoder);
        const QUrl newSource = m_nextSource;
        m_nextSource.clear();

        plock.unlock();

        qCDebug(dragonsdlDecode) << "decode thread gapless transition, emitting signal";
        Q_EMIT gaplessTransition(newSource);
    } else {
        plock.unlock();

        qCDebug(dragonsdlDecode) << "decode thread no pre-warmed decoder, emitting finished signal";
        Q_EMIT finished(false);
        Q_EMIT sessionFinished(sessionGeneration, false);
    }
}

void DragonDecodePipeline::stopSession()
{
    bool stoppable = m_sessionStopSource.request_stop();
    qCDebug(dragonsdlDecode) << "stopSession() called requesting decode session stop" << stoppable;

    if (m_preWarmThread.joinable()) {
        stoppable = m_preWarmThread.request_stop();
        qCDebug(dragonsdlDecode) << "stopSession() cancelling pre-warm thread" << stoppable;
    }

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonsdlDecode) << "stopSession() waiting for decodeLoopActive=false (current=" << m_decodeLoopActive << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonsdlDecode) << "stopSession() decode loop finished";
        m_activeDecoder.reset();
    }

    if (m_preWarmThread.joinable()) {
        qCDebug(dragonsdlDecode) << "stopSession() joining pre-warm thread";
        m_preWarmThread.join();
    }
    qCDebug(dragonsdlDecode) << "stopSession() complete, thread still alive";
}

void DragonDecodePipeline::stop()
{
    qCDebug(dragonsdlDecode) << "stop() called FULL teardown";
    m_sessionStopSource.request_stop();
    m_decodeThread.request_stop();

    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
    }

    m_decoderAssignedCv.notify_all();

    if (m_decodeThread.joinable()) {
        qCDebug(dragonsdlDecode) << "stop() joining decode thread";
        m_decodeThread.join();
        qCDebug(dragonsdlDecode) << "stop() decode thread joined";
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.join();
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_activeDecoder.reset();
        m_preWarmedDecoder.reset();
        m_decodeLoopActive = false;
    }

    if (m_radioStream) {
        m_radioStream->stop();
        m_radioStream.reset();
    }
    qCDebug(dragonsdlDecode) << "stop() full teardown complete";
}

void DragonDecodePipeline::setSource(const QUrl &source, uint64_t generation)
{
    qCDebug(dragonsdlDecode) << "setSource(" << source.toString() << ") gen=" << generation;
    m_generation = generation;

    qCDebug(dragonsdlDecode) << "setSource thread joinable=" << m_decodeThread.joinable();
    if (!m_decodeThread.joinable()) {
        qCDebug(dragonsdlDecode) << "setSource thread is dead, restarting it";
        startDecodeThread();
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_preWarmedDecoder.reset();
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }

    qCDebug(dragonsdlDecode) << "setSource requesting decode session stop";
    m_sessionStopSource.request_stop();

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonsdlDecode) << "setSource waiting for decodeLoopActive=false (current=" << m_decodeLoopActive << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonsdlDecode) << "setSource decode loop inactive, resetting activeDecoder";
        m_activeDecoder.reset();
    }

    qCDebug(dragonsdlDecode) << "setSource creating new decoder";
    auto decoder = createDecoder(source, false, generation);
    if (!decoder) {
        qCDebug(dragonsdlDecode) << "setSource decoder creation FAILED";
        return;
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_activeDecoder = std::move(decoder);
    }

    qCDebug(dragonsdlDecode) << "setSource new decoder installed, creating fresh stop source and notifying";
    m_sessionStopSource = std::stop_source{};
    m_decoderAssignedCv.notify_one();
    qCDebug(dragonsdlDecode) << "setSource notify_one() called, returning";
}

void DragonDecodePipeline::setNextSource(const QUrl &next, uint64_t generation)
{
    m_nextSource = next;

    {
        std::scoped_lock lock(m_decoderMutex);
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
        pthread_setname_np(pthread_self(), "dragon-prewarm");
        auto decoder = createDecoder(next, true, generation);
        if (st.stop_requested()) {
            return;
        }
        if (decoder) {
            std::scoped_lock lock(m_decoderMutex);
            if (!st.stop_requested()) {
                m_preWarmedDecoder = std::move(decoder);
            }
        }
    });
}

bool DragonDecodePipeline::isActive() const
{
    std::scoped_lock lock(m_decoderMutex);
    return m_activeDecoder != nullptr;
}

bool DragonDecodePipeline::hasFatalError() const
{
    std::scoped_lock lock(m_decoderMutex);
    return m_activeDecoder && m_activeDecoder->hasFatalError();
}

void DragonDecodePipeline::requestSeek(int64_t posMs)
{
    std::scoped_lock lock(m_decoderMutex);
    if (m_activeDecoder) {
        m_activeDecoder->requestSeek(posMs);
    }
}

std::unique_ptr<DragonDecoder> DragonDecodePipeline::createDecoder(const QUrl &source, bool, uint64_t generation)
{
    const bool isLocal = source.isLocalFile();

    if (!isLocal) {
        if (m_radioStream) {
            m_radioStream->stop();
            m_radioStream.reset();
        }
        m_radioStream = std::make_unique<DragonRadioStream>();
        m_radioStream->setUrl(source);

        connect(m_radioStream.get(), &DragonRadioStream::errorOccurred, this, [this](const QString &) {
            Q_EMIT errorOccurred(QStringLiteral("Network error"));
            Q_EMIT sessionError(m_generation, QStringLiteral("Network error"));
        });

        connect(m_radioStream.get(), &DragonRadioStream::metadataReady, m_player, &DragonPlayer::currentPlayingForRadiosChanged);
        m_radioStream->start();
    } else if (m_radioStream) {
        m_radioStream->stop();
        m_radioStream.reset();
    }

    DragonDecoder::ReadCallback readCb;
    if (!isLocal) {
        readCb = [this](const std::span<uint8_t> buf) -> int {
            return m_radioStream ? m_radioStream->read(buf, m_sessionStopSource.get_token()) : -1;
        };
    }

    auto decoder = std::make_unique<DragonDecoder>(std::move(readCb), isLocal ? source.toLocalFile() : QString{});

    connect(decoder.get(), &DragonDecoder::streamError, this, [this, generation](const QString &msg) {
        qCDebug(dragonsdlDecode) << "Decoder mid-stream error:" << msg;
        Q_EMIT errorOccurred(msg);
        Q_EMIT sessionError(generation, msg);
    });

    return decoder;
}

uint64_t DragonDecodePipeline::generation() const
{
    return m_generation;
}

void DragonDecodePipeline::setSamplesCallback(SamplesCallback callback)
{
    m_samplesCallback = std::move(callback);
}

const std::unique_ptr<DragonDecoder> &DragonDecodePipeline::activeDecoder() const
{
    return m_activeDecoder;
}

bool DragonDecodePipeline::decodeLoopActive() const
{
    return m_decodeLoopActive;
}
