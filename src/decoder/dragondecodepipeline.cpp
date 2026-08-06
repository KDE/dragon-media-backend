/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragondecodepipeline.h"

#include "dragondecoder.h"
#include "stream/dragonbufferprogress.h"
#include "stream/dragonstream.h"
#include "stream/dragonstreamfactory.h"
#include <DragonMultimedia/dragonicymetadata.h>
#include <DragonMultimedia/dragonplayer.h>

#include "dragonmultimedia_decode_logging.h"
#include "dragonmultimedia_logging.h"

#include <KLocalizedString>
#include <QMetaObject>
#include <QObject>

#include "dragonthreadname.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

using namespace DragonMultimedia;
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

DragonDecodePipeline::DragonDecodePipeline(DragonPlayer *player)
    : QObject(player)
    , m_player(player)
{
    startDecodeThread();
}

void DragonDecodePipeline::startDecodeThread()
{
    qCDebug(dragonMultimediaDecode) << "startDecodeThread() creating new decode thread";
    m_decodeThread = std::jthread([this](std::stop_token st) {
        DragonThreadName::set("dragon-decode");
        qCDebug(dragonMultimediaDecode) << "decode thread started";

        while (!st.stop_requested()) {
            if (!waitForDecoderAssignment(st)) {
                break;
            }

            auto [wasStopped, hadFatalError] = executeDecodeSession();
            if (wasStopped) {
                qCDebug(dragonMultimediaDecode) << "decode thread session was stopped, continue to wait for new decoder";
                continue;
            }

            if (hadFatalError) {
                QUrl errorSource;
                {
                    std::scoped_lock lock(m_decoderMutex);
                    errorSource = m_currentSource;
                }
                qCDebug(dragonMultimediaDecode) << "decode thread fatal error, emitting sessionFinished for" << errorSource.toString();
                Q_EMIT sessionFinished(errorSource, true);
                continue;
            }

            if (st.stop_requested()) {
                qCDebug(dragonMultimediaDecode) << "decode thread outer stop after loop, breaking";
                break;
            }

            processDecodeCompletion();
        }

        qCDebug(dragonMultimediaDecode) << "decode thread exiting outer loop";
    });
}

DragonDecodePipeline::~DragonDecodePipeline()
{
    stop();
}

bool DragonDecodePipeline::waitForDecoderAssignment(std::stop_token st)
{
    std::unique_lock lock(m_decoderMutex);
    qCDebug(dragonMultimediaDecode) << "decode thread waiting for decoder... activeDecoder=" << (m_activeDecoder != nullptr);

    m_decoderAssignedCv.wait(lock, st, [this]() {
        return m_activeDecoder != nullptr;
    });

    qCDebug(dragonMultimediaDecode) << "decode thread woke up activeDecoder=" << (m_activeDecoder != nullptr)
                                    << " outerStopRequested=" << st.stop_requested();

    if (st.stop_requested()) {
        qCDebug(dragonMultimediaDecode) << "decode thread outer loop stop requested, breaking";
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
        qCDebug(dragonMultimediaDecode) << "decode thread decoder became null before decodeLoop, treating as stopped";
        m_decodeLoopFinishedCv.notify_all();
        return {true, false};
    }

    qCDebug(dragonMultimediaDecode) << "decode thread initializing decoder" << decoder;

    InitResult initResult = decoder->initialize();
    initResult.isGapless = false;

    if (completion) {
        completion->setResult(initResult);
    }

    bool hadFatalError = !initResult.success;

    if (initResult.success) {
        qCDebug(dragonMultimediaDecode) << "decode thread starting decodeLoop for decoder" << decoder;

        for (auto event : decoder->decodeLoop(m_sessionStopSource.get_token())) {
            if (m_sessionStopSource.get_token().stop_requested()) {
                qCDebug(dragonMultimediaDecode) << "decode thread stop requested during iteration";
                break;
            }

            std::visit(overloaded{[&](const FormatReady &) {
                                      qCWarning(dragonMultimediaDecode) << "decode thread unexpected FormatReady yielded";
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
                                      qCDebug(dragonMultimediaDecode) << "decode thread DecodeError:" << err.message;
                                      Q_EMIT sessionError(err.message);
                                      hadFatalError = true;
                                  },

                                  [&](const DecodeEof &) {
                                      qCDebug(dragonMultimediaDecode) << "decode thread DecodeEof received";
                                  }},
                       event);
        }

        if (!hadFatalError) {
            if (decoder->hasFatalError()) {
                hadFatalError = true;
            }
        }
    }

    qCDebug(dragonMultimediaDecode) << "decode thread decodeLoop finished";
    const bool decoderHadFatalError = decoder->hasFatalError();
    bool wasStopped;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_decodeLoopActive = false;

        wasStopped = m_sessionStopSource.stop_requested();
        qCDebug(dragonMultimediaDecode) << "decode thread loopInactive, wasStopped=" << wasStopped << " hadFatalError=" << hadFatalError;

        m_activeDecoder.reset();
    }
    m_decodeLoopFinishedCv.notify_all();

    return {wasStopped, hadFatalError || decoderHadFatalError};
}

void DragonDecodePipeline::processDecodeCompletion()
{
    qCDebug(dragonMultimediaDecode) << "processDecodeCompletion() called";

    std::unique_lock plock(m_decoderMutex);
    if (m_preWarmedDecoder) {
        m_activeDecoder = std::move(m_preWarmedDecoder);
        const QUrl newSource = m_nextSource;
        m_currentSource = newSource;
        m_nextSource.clear();

        int sampleRate = 0;
        int channels = 0;
        qint64 durationMs = -1;
        if (m_pendingGaplessCompletion && m_pendingGaplessCompletion->isReady()) {
            const auto result = m_pendingGaplessCompletion->result();
            sampleRate = result.sampleRate;
            channels = result.channels;
            durationMs = result.durationMs;
        }

        plock.unlock();

        qCDebug(dragonMultimediaDecode) << "decode thread gapless transition, decoder swapped for" << newSource.toString() << "sr=" << sampleRate
                                        << "ch=" << channels << "duration=" << durationMs;
        Q_EMIT gaplessTransition(newSource, sampleRate, channels, durationMs);
    } else {
        const QUrl finishedSource = m_currentSource;
        plock.unlock();

        qCDebug(dragonMultimediaDecode) << "decode thread no pre-warmed decoder, emitting sessionFinished for" << finishedSource.toString();
        Q_EMIT sessionFinished(finishedSource, false);
    }
}

QCoro::Task<InitResult> DragonDecodePipeline::initializeSession(QUrl source, bool isGapless)
{
    qCDebug(dragonMultimediaDecode) << "initializeSession(" << source.toString() << ") isGapless=" << isGapless;

    auto completion = std::make_shared<DragonCompletion>();

    qCDebug(dragonMultimediaDecode) << "initializeSession thread joinable=" << m_decodeThread.joinable();
    if (!m_decodeThread.joinable()) {
        qCDebug(dragonMultimediaDecode) << "initializeSession thread is dead, restarting it";
        startDecodeThread();
    }

    cancelPreWarm(QStringLiteral("New session started"));

    qCDebug(dragonMultimediaDecode) << "initializeSession requesting decode session stop";
    m_sessionStopSource.request_stop();

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonMultimediaDecode) << "initializeSession waiting for decodeLoopActive=false (current=" << m_decodeLoopActive.load() << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonMultimediaDecode) << "initializeSession decode loop inactive, resetting activeDecoder";
        m_activeDecoder.reset();
    }

    qCDebug(dragonMultimediaDecode) << "initializeSession creating new decoder";
    auto decoder = createDecoder(source, isGapless);
    if (!decoder) {
        qCDebug(dragonMultimediaDecode) << "initializeSession decoder creation FAILED";
        co_return makeErrorResult(i18n("Failed to create decoder"));
    }

    {
        std::scoped_lock lock(m_decoderMutex);
        m_pendingInitCompletion = completion;
        m_currentSource = source;
        m_activeDecoder = std::move(decoder);
        m_sessionStopSource = std::stop_source{};
    }

    qCDebug(dragonMultimediaDecode) << "initializeSession new decoder installed, creating fresh stop source and notifying";

    m_decoderAssignedCv.notify_one();

    auto result = co_await *completion;

    {
        std::shared_ptr<DragonCompletion> keepAlive;
        {
            std::scoped_lock lock(m_decoderMutex);
            keepAlive = m_pendingInitCompletion;
            if (m_pendingInitCompletion == completion) {
                m_pendingInitCompletion.reset();
            }
        }
        Q_UNUSED(keepAlive)
    }
    co_return result;
}

void DragonDecodePipeline::stopSession()
{
    bool stoppable = m_sessionStopSource.request_stop();
    qCDebug(dragonMultimediaDecode) << "stopSession() called requesting decode session stop" << stoppable;

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
        qCDebug(dragonMultimediaDecode) << "stopSession() cancelling pre-warm thread" << stoppable;
    }

    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonMultimediaDecode) << "stopSession() waiting for decodeLoopActive=false (current=" << m_decodeLoopActive.load() << ")";
        m_decodeLoopFinishedCv.wait(lock, [this]() {
            return !m_decodeLoopActive;
        });
        qCDebug(dragonMultimediaDecode) << "stopSession() decode loop finished";
        m_activeDecoder.reset();
    }

    if (m_preWarmThread.joinable()) {
        qCDebug(dragonMultimediaDecode) << "stopSession() joining pre-warm thread";
        m_preWarmThread.join();
    }
    qCDebug(dragonMultimediaDecode) << "stopSession() complete, thread still alive";
}

void DragonDecodePipeline::stop()
{
    qCDebug(dragonMultimediaDecode) << "stop() called FULL teardown";
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
        qCDebug(dragonMultimediaDecode) << "stop() joining decode thread";
        m_decodeThread.join();
        qCDebug(dragonMultimediaDecode) << "stop() decode thread joined";
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

    std::shared_ptr<DragonStream> oldStream;
    {
        std::scoped_lock lock(m_streamMutex);
        oldStream = std::move(m_stream);
        m_stream.reset();
    }
    if (oldStream) {
        oldStream->stop();
    }
    qCDebug(dragonMultimediaDecode) << "stop() full teardown complete";
}

void DragonDecodePipeline::setNextSource(const QUrl &next)
{
    m_nextSource = next;

    cancelPreWarm(QStringLiteral("New pre-warm started"));

    if (next.isEmpty() || !next.isLocalFile()) {
        return;
    }

    auto completion = std::make_shared<DragonCompletion>();
    {
        std::scoped_lock lock(m_decoderMutex);
        m_pendingGaplessCompletion = completion;
    }

    m_preWarmThread = std::jthread([this, next, completion](std::stop_token st) {
        DragonThreadName::set("dragon-prewarm");
        auto decoder = createDecoder(next, true);
        if (st.stop_requested()) {
            if (completion) {
                completion->cancel(QStringLiteral("Pre-warm stopped"));
            }
            return;
        }
        if (!decoder) {
            if (completion) {
                completion->setResult(makeErrorResult(i18n("Failed to create decoder"), true));
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

        if (completion) {
            completion->setResult(result);
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

void DragonDecodePipeline::requestSeek(qint64 posMs)
{
    std::scoped_lock lock(m_decoderMutex);
    if (m_activeDecoder) {
        m_activeDecoder->requestSeek(posMs);
    }
}

std::unique_ptr<DragonDecoder> DragonDecodePipeline::createDecoder(const QUrl &source, bool)
{
    const bool isLocal = source.isLocalFile();

    std::shared_ptr<DragonStream> oldStream;
    {
        std::scoped_lock lock(m_streamMutex);
        oldStream = std::move(m_stream);
        m_stream = DragonStreamFactory::createStream(source);
    }
    if (oldStream) {
        disconnect(oldStream->bufferProgress(), nullptr, this, nullptr);
        oldStream->stop();
    }

    // Snapshot held by the callbacks below. The shared_ptr keeps the stream
    // alive on the decode thread even if the main/prewarm thread replaces
    // m_stream mid-decode, eliminating the use-after-free that a raw capture
    // would have.
    const std::shared_ptr<DragonStream> stream = [this]() {
        std::scoped_lock lock(m_streamMutex);
        return m_stream;
    }();

    DragonDecoder::ReadCallback readCb;
    DragonDecoder::SeekCallback seekCb;

    if (stream) {
        stream->setUrl(source);

        connect(stream.get(), &DragonStream::errorOccurred, this, [this](const QString &message) {
            Q_EMIT sessionError(message.isEmpty() ? i18n("Stream error") : message);
        });

        connect(stream.get(), &DragonStream::metadataReady, m_player, &DragonPlayer::currentPlayingForRadiosChanged);
        connect(stream->bufferProgress(), &DragonBufferProgress::progressChanged, this, &DragonDecodePipeline::bufferProgressChanged);

        connect(stream.get(), &DragonStream::streamStalled, this, &DragonDecodePipeline::streamStalled, Qt::QueuedConnection);
        connect(stream.get(), &DragonStream::streamBuffering, this, &DragonDecodePipeline::streamBuffering, Qt::QueuedConnection);
        connect(stream.get(), &DragonStream::streamBuffered, this, &DragonDecodePipeline::streamBuffered, Qt::QueuedConnection);

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

    auto decoder =
        std::make_unique<DragonDecoder>(std::move(readCb), std::move(seekCb), stream ? stream->size() : -1, isLocal ? source.toLocalFile() : QString{});

    connect(decoder.get(), &DragonDecoder::streamError, this, [this](const QString &msg) {
        qCDebug(dragonMultimediaDecode) << "Decoder mid-stream error:" << msg;
        Q_EMIT sessionError(msg);
    });

    return decoder;
}

void DragonDecodePipeline::cancelPreWarm(const QString &reason)
{
    std::shared_ptr<DragonCompletion> pending;
    {
        std::scoped_lock lock(m_decoderMutex);
        m_preWarmedDecoder.reset();
        pending = m_pendingGaplessCompletion;
    }
    if (pending) {
        pending->cancel(reason);
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

qint64 DragonDecodePipeline::streamSize() const
{
    std::scoped_lock lock(m_streamMutex);
    return m_stream ? m_stream->size() : -1;
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

bool DragonDecodePipeline::decodeLoopActive() const
{
    return m_decodeLoopActive.load(std::memory_order_acquire);
}
