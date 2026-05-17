/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragondecodepipeline.h"

#include "dragondecoder.h"
#include "dragonkiostream.h"
#include "dragonradiostream.h"
#include <dragonbufferprogress.h>
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
using namespace Qt::StringLiterals;

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

            auto [wasStopped, hadFatalError] = executeDecodeSession();
            if (wasStopped) {
                qCDebug(dragonsdlDecode) << "decode thread session was stopped, continue to wait for new decoder";
                continue;
            }

            if (hadFatalError) {
                QUrl errorSource;
                {
                    std::scoped_lock lock(m_decoderMutex);
                    errorSource = m_currentSource;
                }
                qCDebug(dragonsdlDecode) << "decode thread fatal error, emitting sessionFinished for" << errorSource.toString();
                Q_EMIT sessionFinished(errorSource, true);
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

std::pair<bool, bool> DragonDecodePipeline::executeDecodeSession()
{
    DragonDecoder *decoder = nullptr;
    std::shared_ptr<DragonCompletion> completion;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_decodeLoopActive = true;
        decoder = m_activeDecoder.get();
        completion = m_pendingInitCompletion;
        if (!decoder) {
            m_decodeLoopActive = false;
        }
    }

    if (!decoder) {
        qCDebug(dragonsdlDecode) << "decode thread decoder became null before decodeLoop, treating as stopped";
        m_decodeLoopFinishedCv.notify_all();
        return {true, false};
    }

    qCDebug(dragonsdlDecode) << "decode thread initializing decoder" << decoder;

    InitResult initResult = decoder->initialize();
    initResult.isGapless = false;

    if (completion) {
        completion->setResult(initResult);
    }

    bool hadFatalError = !initResult.success;

    if (initResult.success) {
        qCDebug(dragonsdlDecode) << "decode thread starting decodeLoop for decoder" << decoder;

        for (auto event : decoder->decodeLoop(m_sessionStopSource.get_token())) {
            if (m_sessionStopSource.get_token().stop_requested()) {
                qCDebug(dragonsdlDecode) << "decode thread stop requested during iteration";
                break;
            }

            std::visit(overloaded{[&](const FormatReady &) {
                                      qCWarning(dragonsdlDecode) << "decode thread unexpected FormatReady yielded";
                                  },

                                  [&](const SamplesChunk &sc) {
                                      if (m_samplesCallback && !sc.data.empty()) {
                                          m_samplesCallback(sc.data, m_sessionStopSource.get_token());
                                      }
                                  },

                                  [&](const DecodeError &err) {
                                      qCDebug(dragonsdlDecode) << "decode thread DecodeError:" << err.message;
                                      Q_EMIT sessionError(err.message);
                                      hadFatalError = true;
                                  },

                                  [&](const DecodeEof &) {
                                      qCDebug(dragonsdlDecode) << "decode thread DecodeEof received";
                                  }},
                       event);
        }

        if (!hadFatalError) {
            if (decoder->hasFatalError()) {
                hadFatalError = true;
            }
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
    qCDebug(dragonsdlDecode) << "processDecodeCompletion() called";

    std::unique_lock plock(m_decoderMutex);
    if (m_preWarmedDecoder) {
        m_activeDecoder = std::move(m_preWarmedDecoder);
        const QUrl newSource = m_nextSource;
        m_currentSource = newSource;
        m_nextSource.clear();

        int sampleRate = 0;
        int channels = 0;
        qint64 durationMs = -1;
        if (m_pendingGaplessCompletion && m_pendingGaplessCompletion->await_ready()) {
            const auto result = m_pendingGaplessCompletion->await_resume();
            sampleRate = result.sampleRate;
            channels = result.channels;
            durationMs = result.durationMs;
        }

        plock.unlock();

        qCDebug(dragonsdlDecode) << "decode thread gapless transition, decoder swapped for" << newSource.toString() << "sr=" << sampleRate
                                 << "ch=" << channels << "duration=" << durationMs;
        Q_EMIT gaplessTransition(newSource, sampleRate, channels, durationMs);
    } else {
        const QUrl finishedSource = m_currentSource;
        plock.unlock();

        qCDebug(dragonsdlDecode) << "decode thread no pre-warmed decoder, emitting sessionFinished for" << finishedSource.toString();
        Q_EMIT sessionFinished(finishedSource, false);
    }
}

QCoro::Task<InitResult> DragonDecodePipeline::initializeSession(QUrl source, bool isGapless)
{
    qCDebug(dragonsdlDecode) << "initializeSession(" << source.toString() << ") isGapless=" << isGapless;

    auto completion = std::make_shared<DragonCompletion>(this);

    qCDebug(dragonsdlDecode) << "initializeSession thread joinable=" << m_decodeThread.joinable();
    if (!m_decodeThread.joinable()) {
        qCDebug(dragonsdlDecode) << "initializeSession thread is dead, restarting it";
        startDecodeThread();
    }

    cancelPreWarm(QStringLiteral("New session started"));

    qCDebug(dragonsdlDecode) << "initializeSession requesting decode session stop";
    m_sessionStopSource.request_stop();

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonsdlDecode) << "initializeSession waiting for decodeLoopActive=false (current=" << m_decodeLoopActive << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonsdlDecode) << "initializeSession decode loop inactive, resetting activeDecoder";
        m_activeDecoder.reset();
    }

    qCDebug(dragonsdlDecode) << "initializeSession creating new decoder";
    auto decoder = createDecoder(source, isGapless);
    if (!decoder) {
        qCDebug(dragonsdlDecode) << "initializeSession decoder creation FAILED";
        co_return makeErrorResult(QStringLiteral("Failed to create decoder"));
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_pendingInitCompletion = completion;
        m_currentSource = source;
        m_activeDecoder = std::move(decoder);
        m_sessionStopSource = std::stop_source{};
    }

    qCDebug(dragonsdlDecode) << "initializeSession new decoder installed, creating fresh stop source and notifying";

    m_decoderAssignedCv.notify_one();

    auto result = co_await *completion;

    {
        std::scoped_lock lock(m_decoderMutex);
        if (m_pendingInitCompletion == completion) {
            m_pendingInitCompletion.reset();
        }
    }
    co_return result;
}

void DragonDecodePipeline::stopSession()
{
    bool stoppable = m_sessionStopSource.request_stop();
    qCDebug(dragonsdlDecode) << "stopSession() called requesting decode session stop" << stoppable;

    {
        std::shared_ptr<DragonCompletion> pending;
        {
            std::scoped_lock lock(m_decoderMutex);
            pending = m_pendingInitCompletion;
        }
        if (pending) {
            pending->cancel(QStringLiteral("Session stopped"));
        }
    }

    {
        std::shared_ptr<DragonCompletion> pending;
        {
            std::scoped_lock lock(m_decoderMutex);
            pending = m_pendingGaplessCompletion;
        }
        if (pending) {
            pending->cancel(QStringLiteral("Session stopped"));
        }
    }

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

    {
        std::shared_ptr<DragonCompletion> pending;
        {
            std::scoped_lock lock(m_decoderMutex);
            pending = m_pendingInitCompletion;
        }
        if (pending) {
            pending->cancel(QStringLiteral("Full stop"));
        }
    }

    {
        std::shared_ptr<DragonCompletion> pending;
        {
            std::scoped_lock lock(m_decoderMutex);
            pending = m_pendingGaplessCompletion;
        }
        if (pending) {
            pending->cancel(QStringLiteral("Full stop"));
        }
    }

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
    if (m_kioStream) {
        m_kioStream->stop();
        m_kioStream.reset();
    }
    qCDebug(dragonsdlDecode) << "stop() full teardown complete";
}

void DragonDecodePipeline::setNextSource(const QUrl &next)
{
    m_nextSource = next;

    cancelPreWarm(QStringLiteral("New pre-warm started"));

    if (next.isEmpty() || !next.isLocalFile()) {
        return;
    }

    m_pendingGaplessCompletion = std::make_shared<DragonCompletion>(this);

    m_preWarmThread = std::jthread([this, next](std::stop_token st) {
        pthread_setname_np(pthread_self(), "dragon-prewarm");
        auto decoder = createDecoder(next, true);
        if (st.stop_requested()) {
            if (m_pendingGaplessCompletion) {
                m_pendingGaplessCompletion->cancel(QStringLiteral("Pre-warm stopped"));
            }
            return;
        }
        if (!decoder) {
            if (m_pendingGaplessCompletion) {
                m_pendingGaplessCompletion->setResult(makeErrorResult(QStringLiteral("Failed to create decoder"), true));
            }
            return;
        }

        InitResult result = decoder->initialize();
        result.isGapless = true;

        if (st.stop_requested()) {
            result = makeCancelledResult(QStringLiteral("Pre-warm cancelled"), true);
        } else if (result.success) {
            std::scoped_lock lock(m_decoderMutex);
            if (!st.stop_requested()) {
                m_preWarmedDecoder = std::move(decoder);
            } else {
                result = makeCancelledResult(QStringLiteral("Pre-warm cancelled"), true);
            }
        }

        if (m_pendingGaplessCompletion) {
            m_pendingGaplessCompletion->setResult(result);
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

std::unique_ptr<DragonDecoder> DragonDecodePipeline::createDecoder(const QUrl &source, bool)
{
    const bool isLocal = source.isLocalFile();
    const QString scheme = source.scheme();
    const bool isHttp = scheme == u"http"_s || scheme == u"https"_s;
    const bool isKio = !isLocal && !isHttp;

    if (m_radioStream) {
        disconnect(m_radioStream->bufferProgress(), nullptr, this, nullptr);
        m_radioStream->stop();
        m_radioStream.reset();
    }
    if (m_kioStream) {
        disconnect(m_kioStream->bufferProgress(), nullptr, this, nullptr);
        m_kioStream->stop();
        m_kioStream.reset();
    }

    if (isHttp) {
        m_radioStream = std::make_unique<DragonRadioStream>();
        m_radioStream->setUrl(source);

        connect(m_radioStream.get(), &DragonRadioStream::errorOccurred, this, [this](const QString &) {
            Q_EMIT sessionError(QStringLiteral("Network error"));
        });

        connect(m_radioStream.get(), &DragonRadioStream::metadataReady, m_player, &DragonPlayer::currentPlayingForRadiosChanged);
        connect(m_radioStream->bufferProgress(), &DragonBufferProgress::progressChanged, this, &DragonDecodePipeline::bufferProgressChanged);
        m_radioStream->start();
    } else if (isKio) {
        m_kioStream = std::make_unique<DragonKioStream>();
        m_kioStream->setUrl(source);

        connect(m_kioStream.get(), &DragonKioStream::errorOccurred, this, [this](const QString &) {
            Q_EMIT sessionError(QStringLiteral("KIO error"));
        });

        connect(m_kioStream->bufferProgress(), &DragonBufferProgress::progressChanged, this, &DragonDecodePipeline::bufferProgressChanged);
        m_kioStream->start();
    } else {
        Q_EMIT bufferProgressChanged(1.0);
    }

    DragonDecoder::ReadCallback readCb;
    if (isHttp) {
        readCb = [this](const std::span<uint8_t> buf) -> int {
            return m_radioStream ? m_radioStream->read(buf, m_sessionStopSource.get_token()) : -1;
        };
    } else if (isKio) {
        readCb = [this](const std::span<uint8_t> buf) -> int {
            return m_kioStream ? m_kioStream->read(buf, m_sessionStopSource.get_token()) : -1;
        };
    }

    auto decoder = std::make_unique<DragonDecoder>(std::move(readCb), isLocal ? source.toLocalFile() : QString{});

    connect(decoder.get(), &DragonDecoder::streamError, this, [this](const QString &msg) {
        qCDebug(dragonsdlDecode) << "Decoder mid-stream error:" << msg;
        Q_EMIT sessionError(msg);
    });

    return decoder;
}

void DragonDecodePipeline::cancelPreWarm(const QString &reason)
{
    {
        std::scoped_lock lock(m_decoderMutex);
        m_preWarmedDecoder.reset();
        if (m_pendingGaplessCompletion) {
            m_pendingGaplessCompletion->cancel(reason);
        }
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }
}

void DragonDecodePipeline::setCurrentSource(const QUrl &source)
{
    std::scoped_lock lock(m_decoderMutex);
    m_currentSource = source;
}

void DragonDecodePipeline::setSamplesCallback(SamplesCallback callback)
{
    m_samplesCallback = std::move(callback);
}

InitResult DragonDecodePipeline::makeSuccessResult(const FormatReady &fr, bool isGapless)
{
    InitResult result;
    result.success = true;
    result.sampleRate = fr.sampleRate;
    result.channels = fr.channels;
    result.durationMs = fr.durationMs;
    result.isGapless = isGapless;
    return result;
}

InitResult DragonDecodePipeline::makeErrorResult(const QString &message, bool isGapless)
{
    InitResult result;
    result.success = false;
    result.errorMessage = message;
    result.isGapless = isGapless;
    return result;
}

InitResult DragonDecodePipeline::makeCancelledResult(const QString &message, bool isGapless)
{
    InitResult result;
    result.success = false;
    result.cancelled = true;
    result.errorMessage = message;
    result.isGapless = isGapless;
    return result;
}

const std::unique_ptr<DragonDecoder> &DragonDecodePipeline::activeDecoder() const
{
    return m_activeDecoder;
}

bool DragonDecodePipeline::decodeLoopActive() const
{
    return m_decodeLoopActive;
}
