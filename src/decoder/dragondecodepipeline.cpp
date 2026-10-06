/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragondecodepipeline.h"

#include "dragondecoder.h"
#include "dragonffmpegjni.h"
#include "stream/dragonbufferprogress.h"
#include "stream/dragonstream.h"
#include "stream/dragonstreamfactory.h"
#include <dragonicymetadata.h>
#include <dragonplayer.h>

#include "dragonmediabackend_decode_logging.h"
#include "dragonmediabackend_logging.h"

#include <KLocalizedString>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include "dragonthreadname.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

using namespace DragonMediaBackend;
using namespace Qt::StringLiterals;

namespace
{

static constexpr auto INVOKE_STOPPABLE_TIMEOUT = std::chrono::milliseconds(2000);

template<typename R, typename F>
std::optional<R> invokeStoppable(QObject *context, std::stop_token st, F &&func)
{
    struct State {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::optional<R> result;
        bool done = false;
    };
    auto state = std::make_shared<State>();

    QMetaObject::invokeMethod(
        context,
        [state, func = std::forward<F>(func)]() mutable {
            auto res = func();
            {
                std::scoped_lock lock(state->mutex);
                state->result = std::move(res);
                state->done = true;
            }
            state->cv.notify_one();
        },
        Qt::QueuedConnection);

    std::unique_lock lock(state->mutex);
    const bool completed = state->cv.wait_for(lock, st, INVOKE_STOPPABLE_TIMEOUT, [state]() {
        return state->done;
    });
    if (!completed || !state->done) {
        // Stop requested or timed out waiting for the target thread.
        return std::nullopt;
    }
    return std::move(*state->result);
}

}

DragonDecodePipeline::Session::Session(DragonDecodePipeline *owner, std::shared_ptr<DragonStream> stream)
    : owner(owner)
    , stream(std::move(stream))
{
}

DragonDecodePipeline::Session::~Session()
{
    // The decoder's read/seek callbacks share the stream; drop them first.
    decoder.reset();
    if (!stream) {
        return;
    }

    QObject::disconnect(stream.get(), nullptr, owner, nullptr);
    QObject::disconnect(stream.get(), nullptr, owner->m_player, nullptr);
    QObject::disconnect(stream->bufferProgress(), nullptr, owner, nullptr);
    stream->stop();

    if (QThread::currentThread() != stream->thread()) {
        QMetaObject::invokeMethod(
            owner,
            [retired = stream]() {
                Q_UNUSED(retired);
            },
            Qt::QueuedConnection);
    }
}

DragonDecodePipeline::DragonDecodePipeline(DragonPlayer *player)
    : QObject(player)
    , m_player(player)
{
    startDecodeThread();
}

void DragonDecodePipeline::startDecodeThread()
{
    qCDebug(dragonMediaBackendDecode) << "startDecodeThread() creating new decode thread";
    m_decodeThread = std::jthread([this](std::stop_token st) {
        DragonThreadName::set("dragon-decode");
        qCDebug(dragonMediaBackendDecode) << "decode thread started";

        while (!st.stop_requested()) {
            if (!waitForDecoderAssignment(st)) {
                break;
            }

            auto [wasStopped, hadFatalError] = executeDecodeSession();
            if (wasStopped) {
                qCDebug(dragonMediaBackendDecode) << "decode thread session was stopped, continue to wait for new decoder";
                continue;
            }

            if (hadFatalError) {
                QUrl errorSource;
                {
                    std::scoped_lock lock(m_decoderMutex);
                    errorSource = m_currentSource;
                }
                qCDebug(dragonMediaBackendDecode) << "decode thread fatal error, emitting sessionFinished for" << errorSource.toString();
                Q_EMIT sessionFinished(errorSource, true);
                continue;
            }

            if (st.stop_requested()) {
                qCDebug(dragonMediaBackendDecode) << "decode thread outer stop after loop, breaking";
                break;
            }

            processDecodeCompletion();
        }

        qCDebug(dragonMediaBackendDecode) << "decode thread exiting outer loop";
    });
}

DragonDecodePipeline::~DragonDecodePipeline()
{
    stop();
}

bool DragonDecodePipeline::waitForDecoderAssignment(std::stop_token st)
{
    std::unique_lock lock(m_decoderMutex);
    qCDebug(dragonMediaBackendDecode) << "decode thread waiting for decoder... activeDecoder=" << hasActiveDecoder();

    m_decoderAssignedCv.wait(lock, st, [this]() {
        return hasActiveDecoder();
    });

    qCDebug(dragonMediaBackendDecode) << "decode thread woke up activeDecoder=" << hasActiveDecoder() << " outerStopRequested=" << st.stop_requested();

    if (st.stop_requested()) {
        qCDebug(dragonMediaBackendDecode) << "decode thread outer loop stop requested, breaking";
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
        decoder = hasActiveDecoder() ? m_activeSession->decoder.get() : nullptr;
        completion = m_pendingInitCompletion;
        if (!decoder) {
            m_decodeLoopActive = false;
        }
    }

    if (!decoder) {
        qCDebug(dragonMediaBackendDecode) << "decode thread decoder became null before decodeLoop, treating as stopped";
        m_decodeLoopFinishedCv.notify_all();
        return {true, false};
    }

    qCDebug(dragonMediaBackendDecode) << "decode thread initializing decoder" << decoder;

    InitResult initResult = decoder->initialize();
    initResult.isGapless = false;

    if (completion) {
        completion->setResult(initResult);
    }

    bool hadFatalError = !initResult.success;

    if (initResult.success) {
        qCDebug(dragonMediaBackendDecode) << "decode thread starting decodeLoop for decoder" << decoder;

        for (auto event : decoder->decodeLoop(m_sessionStopSource.get_token())) {
            if (m_sessionStopSource.get_token().stop_requested()) {
                qCDebug(dragonMediaBackendDecode) << "decode thread stop requested during iteration";
                break;
            }

            std::visit(overloaded{[&](const FormatReady &) {
                                      qCWarning(dragonMediaBackendDecode) << "decode thread unexpected FormatReady yielded";
                                  },

                                  [&](const SamplesChunk &sc) {
                                      SamplesCallback callback;
                                      {
                                          std::scoped_lock lock(m_samplesCallbackMutex);
                                          callback = m_samplesCallback;
                                      }
                                      if (callback && !sc.data.empty()) {
                                          callback(sc.data, m_sessionStopSource.get_token());
                                      }
                                  },

                                  [&](const DecodeError &err) {
                                      qCDebug(dragonMediaBackendDecode) << "decode thread DecodeError:" << err.message;
                                      Q_EMIT sessionError(err.message);
                                      hadFatalError = true;
                                  },

                                  [&](const DecodeEof &) {
                                      qCDebug(dragonMediaBackendDecode) << "decode thread DecodeEof received";
                                  }},
                       event);
        }

        if (!hadFatalError) {
            if (decoder->hasFatalError()) {
                hadFatalError = true;
            }
        }
    }

    qCDebug(dragonMediaBackendDecode) << "decode thread decodeLoop finished";
    const bool decoderHadFatalError = decoder->hasFatalError();
    bool wasStopped;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_decodeLoopActive = false;

        wasStopped = m_sessionStopSource.stop_requested();
        qCDebug(dragonMediaBackendDecode) << "decode thread loopInactive, wasStopped=" << wasStopped << " hadFatalError=" << hadFatalError;

        if (m_activeSession) {
            m_activeSession->decoder.reset();
        }
    }
    m_decodeLoopFinishedCv.notify_all();

    return {wasStopped, hadFatalError || decoderHadFatalError};
}

void DragonDecodePipeline::processDecodeCompletion()
{
    qCDebug(dragonMediaBackendDecode) << "processDecodeCompletion() called";

    if (promotePreWarmedDecoder()) {
        return;
    }

    std::unique_lock plock(m_decoderMutex);
    const QUrl finishedSource = m_currentSource;
    plock.unlock();

    qCDebug(dragonMediaBackendDecode) << "decode thread no pre-warmed decoder, emitting sessionFinished for" << finishedSource.toString();
    Q_EMIT sessionFinished(finishedSource, false);
}

bool DragonDecodePipeline::promotePreWarmedDecoder()
{
    std::unique_lock plock(m_decoderMutex);
    if (!m_preWarmedSession || hasActiveDecoder()) {
        return false;
    }

    std::unique_ptr<Session> retired = std::exchange(m_activeSession, std::move(m_preWarmedSession));
    const QUrl newSource = m_nextSource;
    m_currentSource = newSource;
    m_nextSource.clear();

    int sampleRate = 0;
    int channels = 0;
    std::optional<std::chrono::milliseconds> duration{};
    if (m_pendingGaplessCompletion && m_pendingGaplessCompletion->isReady()) {
        const auto result = m_pendingGaplessCompletion->result();
        sampleRate = result.sampleRate;
        channels = result.channels;
        duration = result.duration;
    }

    plock.unlock();

    // Stops the finished track's stream (closing its device, if any).
    retired.reset();

    m_decoderAssignedCv.notify_all();

    qCDebug(dragonMediaBackendDecode) << "gapless transition, decoder swapped for" << newSource.toString() << "sr=" << sampleRate << "ch=" << channels
                                      << "duration=" << duration.value_or(std::chrono::milliseconds{-1}).count() << "ms";
    Q_EMIT gaplessTransition(newSource, sampleRate, channels, duration);
    return true;
}

QCoro::Task<InitResult> DragonDecodePipeline::initializeSession(QUrl source, bool isGapless)
{
    qCDebug(dragonMediaBackendDecode) << "initializeSession(" << source.toString() << ") isGapless=" << isGapless;

    auto completion = std::make_shared<DragonCompletion>();

    qCDebug(dragonMediaBackendDecode) << "initializeSession thread joinable=" << m_decodeThread.joinable();
    if (!m_decodeThread.joinable()) {
        qCDebug(dragonMediaBackendDecode) << "initializeSession thread is dead, restarting it";
        startDecodeThread();
    }

    cancelPreWarm(QStringLiteral("New session started"));

    qCDebug(dragonMediaBackendDecode) << "initializeSession requesting decode session stop";
    m_sessionStopSource.request_stop();

    std::unique_ptr<Session> retired;
    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonMediaBackendDecode) << "initializeSession waiting for decodeLoopActive=false (current=" << m_decodeLoopActive.load() << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonMediaBackendDecode) << "initializeSession decode loop inactive, retiring active session";
        retired = std::move(m_activeSession);
    }
    // Stop the old stream before the new one starts, so a device it holds
    // is released first.
    retired.reset();

    qCDebug(dragonMediaBackendDecode) << "initializeSession creating new decoder";
    auto session = createSession(source, false);
    if (!session) {
        qCDebug(dragonMediaBackendDecode) << "initializeSession decoder creation FAILED";
        co_return makeErrorResult(i18n("Failed to create decoder"));
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_pendingInitCompletion = completion;
        m_currentSource = source;
        m_activeSession = std::move(session);
        m_sessionStopSource = std::stop_source{};
    }

    qCDebug(dragonMediaBackendDecode) << "initializeSession new decoder installed, creating fresh stop source and notifying";

    m_decoderAssignedCv.notify_one();

    co_return co_await *completion;
}

void DragonDecodePipeline::stopSession()
{
    bool stoppable = m_sessionStopSource.request_stop();
    qCDebug(dragonMediaBackendDecode) << "stopSession() called requesting decode session stop" << stoppable;

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
        qCDebug(dragonMediaBackendDecode) << "stopSession() cancelling pre-warm thread" << stoppable;
    }

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonMediaBackendDecode) << "stopSession() waiting for decodeLoopActive=false (current=" << m_decodeLoopActive.load() << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonMediaBackendDecode) << "stopSession() decode loop finished";
        if (m_activeSession) {
            m_activeSession->decoder.reset();
        }
    }

    if (m_preWarmThread.joinable()) {
        qCDebug(dragonMediaBackendDecode) << "stopSession() joining pre-warm thread";
        m_preWarmThread.join();
    }
    qCDebug(dragonMediaBackendDecode) << "stopSession() complete, thread still alive";
}

void DragonDecodePipeline::stop()
{
    qCDebug(dragonMediaBackendDecode) << "stop() called FULL teardown";
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
        qCDebug(dragonMediaBackendDecode) << "stop() joining decode thread";
        m_decodeThread.join();
        qCDebug(dragonMediaBackendDecode) << "stop() decode thread joined";
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.join();
    }

    std::unique_ptr<Session> activeSession;
    std::unique_ptr<Session> preWarmedSession;
    {
        std::scoped_lock lock(m_decoderMutex);
        activeSession = std::move(m_activeSession);
        preWarmedSession = std::move(m_preWarmedSession);
        m_decodeLoopActive = false;
    }
    activeSession.reset();
    preWarmedSession.reset();
    qCDebug(dragonMediaBackendDecode) << "stop() full teardown complete";
}

void DragonDecodePipeline::setNextSource(const QUrl &next)
{
    {
        std::scoped_lock lock(m_decoderMutex);
        m_nextSource = next;
    }

    cancelPreWarm(QStringLiteral("New pre-warm started"));

    if (next.isEmpty() || !DragonStreamFactory::supportsGaplessPreWarm(next)) {
        return;
    }

    auto completion = std::make_shared<DragonCompletion>();
    {
        std::scoped_lock lock(m_decoderMutex);
        m_pendingGaplessCompletion = completion;
    }

    m_preWarmThread = std::jthread([this, next, completion](std::stop_token st) {
        DragonThreadName::set("dragon-prewarm");
        auto session = createSession(next, true);
        if (st.stop_requested()) {
            if (completion) {
                completion->cancel(QStringLiteral("Pre-warm stopped"));
            }
            return;
        }
        if (!session) {
            if (completion) {
                completion->setResult(makeErrorResult(i18n("Failed to create decoder"), true));
            }
            return;
        }

        InitResult result = session->decoder->initialize();
        result.isGapless = true;

        if (st.stop_requested()) {
            result = makeCancelledResult(QStringLiteral("Pre-warm cancelled"), true);
        } else if (result.success) {
            std::scoped_lock lock(m_decoderMutex);
            if (!st.stop_requested()) {
                m_preWarmedSession = std::move(session);
            } else {
                result = makeCancelledResult(QStringLiteral("Pre-warm cancelled"), true);
            }
        }

        if (completion) {
            completion->setResult(result);
        }
    });
}

bool DragonDecodePipeline::hasActiveDecoder() const
{
    return m_activeSession && m_activeSession->decoder;
}

bool DragonDecodePipeline::isActive() const
{
    std::scoped_lock lock(m_decoderMutex);
    return hasActiveDecoder();
}

bool DragonDecodePipeline::hasFatalError() const
{
    std::scoped_lock lock(m_decoderMutex);
    return hasActiveDecoder() && m_activeSession->decoder->hasFatalError();
}

void DragonDecodePipeline::requestSeek(std::chrono::milliseconds position)
{
    std::scoped_lock lock(m_decoderMutex);
    if (hasActiveDecoder()) {
        m_activeSession->decoder->requestSeek(position);
    }
}

std::unique_ptr<DragonDecodePipeline::Session> DragonDecodePipeline::createSession(const QUrl &source, bool isPreWarm)
{
    initAndroidJniBridge();

    const bool isLocal = DragonStreamFactory::isLocalSource(source);

    auto session = std::make_unique<Session>(this, DragonStreamFactory::createStream(source));

    const std::shared_ptr<DragonStream> &stream = session->stream;

    DragonDecoder::ReadCallback readCb;
    DragonDecoder::SeekCallback seekCb;

    if (stream) {
        stream->setUrl(source);

        if (isPreWarm && stream->thread() != thread()) {
            stream->moveToThread(thread());
        }

        connect(stream.get(), &DragonStream::errorOccurred, this, [this](const QString &message) {
            Q_EMIT sessionError(message.isEmpty() ? i18n("Stream error") : message);
        });

        connect(stream.get(), &DragonStream::metadataReady, m_player, &DragonPlayer::currentPlayingForRadiosChanged);
        connect(stream->bufferProgress(), &DragonBufferProgress::progressChanged, this, &DragonDecodePipeline::bufferProgressChanged);

        connect(stream.get(), &DragonStream::streamStalled, this, &DragonDecodePipeline::streamStalled, Qt::QueuedConnection);
        connect(stream.get(), &DragonStream::streamBuffering, this, &DragonDecodePipeline::streamBuffering, Qt::QueuedConnection);
        connect(stream.get(), &DragonStream::streamBuffered, this, &DragonDecodePipeline::streamBuffered, Qt::QueuedConnection);
        connect(stream.get(), &DragonStream::seekableChanged, this, &DragonDecodePipeline::streamSeekableChanged, Qt::QueuedConnection);

        stream->start();

        readCb = [stream, this](const std::span<uint8_t> buf) -> int {
            return stream->read(buf, m_sessionStopSource.get_token());
        };

        seekCb = [stream, this](qint64 offset, DragonDecoder::SeekWhence whence) -> qint64 {
            auto result = invokeStoppable<qint64>(stream.get(), m_sessionStopSource.get_token(), [stream, offset, whence]() {
                if (whence == DragonDecoder::SeekWhence::Set) {
                    return stream->seek(offset);
                } else if (whence == DragonDecoder::SeekWhence::Cur) {
                    return stream->seek(stream->position() + offset);
                } else if (whence == DragonDecoder::SeekWhence::End) {
                    const qint64 sz = stream->size();
                    if (sz > 0) {
                        return stream->seek(sz + offset);
                    }
                }
                return static_cast<qint64>(-1);
            });

            return result.value_or(-1);
        };
    } else {
        Q_EMIT bufferProgressChanged(1.0);
    }

    const QString decoderPath = [&]() -> QString {
        if (!isLocal) {
            return {};
        }
        return source.isLocalFile() ? source.toLocalFile() : source.toString(QUrl::FullyEncoded);
    }();

    session->decoder = std::make_unique<DragonDecoder>(std::move(readCb), std::move(seekCb), stream ? stream->size() : -1, decoderPath);

    connect(session->decoder.get(), &DragonDecoder::streamError, this, [this](const QString &msg) {
        qCDebug(dragonMediaBackendDecode) << "Decoder mid-stream error:" << msg;
        Q_EMIT sessionError(msg);
    });

    return session;
}

void DragonDecodePipeline::cancelPreWarm(const QString &reason)
{
    std::shared_ptr<DragonCompletion> pending;
    std::unique_ptr<Session> preWarmedSession;
    {
        std::scoped_lock lock(m_decoderMutex);
        preWarmedSession = std::move(m_preWarmedSession);
        pending = m_pendingGaplessCompletion;
        m_pendingGaplessCompletion.reset();
    }
    if (pending) {
        pending->cancel(reason);
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }
    preWarmedSession.reset();
}

void DragonDecodePipeline::setCurrentSource(const QUrl &source)
{
    std::scoped_lock lock(m_decoderMutex);
    m_currentSource = source;
}

qint64 DragonDecodePipeline::streamSize() const
{
    std::shared_ptr<DragonStream> stream;
    {
        std::scoped_lock lock(m_decoderMutex);
        if (m_activeSession) {
            stream = m_activeSession->stream;
        }
    }
    return stream ? stream->size() : -1;
}

bool DragonDecodePipeline::streamIsSeekable() const
{
    std::shared_ptr<DragonStream> stream;
    {
        std::scoped_lock lock(m_decoderMutex);
        if (m_activeSession) {
            stream = m_activeSession->stream;
        }
    }
    return stream && stream->isSeekable();
}

void DragonDecodePipeline::setSamplesCallback(SamplesCallback callback)
{
    std::scoped_lock lock(m_samplesCallbackMutex);
    m_samplesCallback = std::move(callback);
}

InitResult DragonDecodePipeline::makeSuccessResult(const FormatReady &fr, bool isGapless)
{
    InitResult result;
    result.success = true;
    result.sampleRate = fr.sampleRate;
    result.channels = fr.channels;
    result.duration = fr.duration;
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

bool DragonDecodePipeline::decodeLoopActive() const
{
    return m_decodeLoopActive.load(std::memory_order_acquire);
}
