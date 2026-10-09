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

DragonDecodePipeline::~DragonDecodePipeline()
{
    stop();
}

void DragonDecodePipeline::startDecodeThread()
{
    qCDebug(dragonMediaBackendDecode) << "startDecodeThread() creating new decode thread";
    m_decodeThread = std::jthread([this](std::stop_token st) {
        DragonThreadName::set("dragon-decode");
        qCDebug(dragonMediaBackendDecode) << "decode thread started";
        decodeThreadMain(st);
        qCDebug(dragonMediaBackendDecode) << "decode thread exiting";
    });
}

void DragonDecodePipeline::decodeThreadMain(std::stop_token st)
{
    std::unique_lock lock(m_decoderMutex);
    while (!st.stop_requested()) {
        Action action = Action::Wait;
        if (!m_stateChanged.wait(lock, st, [&action, this, &st]() {
                return st.stop_requested() || (action = nextAction()) != Action::Wait;
            })) {
            break;
        }
        if (st.stop_requested()) {
            break;
        }

        switch (action) {
        case Action::Decode:
            runCurrentSession(lock);
            break;
        case Action::Promote:
            // The decode-loop-active flag covers the promotion's unlocked
            // boundary-recording window: stopSession()/initializeSession()
            // (which wait for it) cannot tear the session down while a
            // promotion is in flight.
            m_decodeLoopActive = true;
            promoteNext(lock);
            m_decodeLoopActive = false;
            m_stateChanged.notify_all();
            break;
        case Action::Finalize:
            // Same for finalizeSession()'s unlocked sessionFinished
            // emission.
            m_decodeLoopActive = true;
            finalizeSession(lock);
            m_decodeLoopActive = false;
            m_stateChanged.notify_all();
            break;
        case Action::Wait:
            break;
        }
    }
}

DragonDecodePipeline::Action DragonDecodePipeline::nextAction() const
{
    switch (m_phase) {
    case Phase::Starting:
        return Action::Decode;
    case Phase::Finished:
        if (m_next && m_next->ready()) {
            return Action::Promote;
        }
        return (!m_next && !m_endReported) ? Action::Finalize : Action::Wait;
    case Phase::Idle:
    case Phase::Decoding:
    case Phase::Promoting:
    case Phase::Stopped:
        return Action::Wait;
    }
    return Action::Wait;
}

void DragonDecodePipeline::runCurrentSession(std::unique_lock<std::mutex> &lock)
{
    // lock is held here.
    DragonDecoder *decoder = nullptr;
    std::shared_ptr<DragonCompletion> completion;
    bool preInitialized = false;
    FormatReady preWarmFormat{};
    {
        m_decodeLoopActive = true;
        decoder = m_activeSession ? m_activeSession->decoder.get() : nullptr;
        preInitialized = m_activeSession ? m_activeSession->preInitialized : false;
        if (m_activeSession && m_activeSession->format) {
            preWarmFormat = *m_activeSession->format;
        }
        completion = std::exchange(m_startCompletion, nullptr);
        if (decoder != nullptr) {
            m_phase = Phase::Decoding;
        }
    }
    lock.unlock();

    if (!decoder) {
        qCDebug(dragonMediaBackendDecode) << "decode thread session retired before decodeLoop, treating as stopped";
        {
            std::scoped_lock relock(m_decoderMutex);
            m_decodeLoopActive = false;
            m_phase = Phase::Stopped;
        }
        m_stateChanged.notify_all();
        lock.lock();
        return;
    }

    qCDebug(dragonMediaBackendDecode) << "decode thread running session, preInitialized=" << preInitialized;

    InitResult initResult = preInitialized ? makeSuccessResult(preWarmFormat) : [&]() {
        auto result = decoder->initialize();
        result.isGapless = false;
        return result;
    }();

    if (completion && !preInitialized) {
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
                                          std::scoped_lock callbackLock(m_samplesCallbackMutex);
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
    const bool wasStopped = m_sessionStopSource.get_token().stop_requested();
    const bool fatal = hadFatalError || decoderHadFatalError;
    QUrl finishedSource;
    {
        std::scoped_lock relock(m_decoderMutex);
        m_decodeLoopActive = false;

        finishedSource = m_currentSource;
        qCDebug(dragonMediaBackendDecode) << "decode thread loopInactive, wasStopped=" << wasStopped << " fatal=" << fatal;

        if (m_phase == Phase::Decoding) {
            m_phase = (wasStopped || fatal) ? Phase::Stopped : Phase::Finished;
        }
        if (fatal) {
            // The fatal sessionFinished below counts as this session's
            // reported end.
            m_endReported = true;
        }
        if (m_activeSession) {
            m_activeSession->decoder.reset();
        }
    }
    m_stateChanged.notify_all();

    if (fatal) {
        qCDebug(dragonMediaBackendDecode) << "decode thread fatal error, emitting sessionFinished for" << finishedSource.toString();
        Q_EMIT sessionFinished(finishedSource, true);
        lock.lock();
        return;
    }
    if (wasStopped) {
        qCDebug(dragonMediaBackendDecode) << "decode thread session was stopped, no completion event";
        lock.lock();
        return;
    }

    BoundaryCallback boundaryCallback;
    {
        std::scoped_lock callbackLock(m_boundaryCallbackMutex);
        boundaryCallback = m_boundaryCallback;
    }
    if (boundaryCallback) {
        BoundaryEvent event;
        event.kind = BoundaryEvent::Kind::EndOfStream;
        boundaryCallback(event);
    }

    lock.lock();
}

void DragonDecodePipeline::finalizeSession(std::unique_lock<std::mutex> &lock)
{
    QUrl finishedSource;
    if (m_endReported) {
        return;
    }
    m_endReported = true;
    finishedSource = m_currentSource;

    lock.unlock();
    qCDebug(dragonMediaBackendDecode) << "decode thread finalizing track, emitting sessionFinished for" << finishedSource.toString();
    Q_EMIT sessionFinished(finishedSource, false);
    lock.lock();
}

bool DragonDecodePipeline::promoteNext(std::unique_lock<std::mutex> &lock)
{
    // lock is held here; phase is Finished and m_next is ready (nextAction()).
    if (!m_next || !m_next->ready()) {
        return false;
    }

    const QUrl newSource = m_next->url;
    const FormatReady format = *m_next->format;
    std::unique_ptr<Session> claimed = std::move(m_next->session);
    m_next.reset();
    m_phase = Phase::Promoting;
    lock.unlock();

    BoundaryCallback boundaryCallback;
    {
        std::scoped_lock callbackLock(m_boundaryCallbackMutex);
        boundaryCallback = m_boundaryCallback;
    }
    BoundaryEvent boundaryEvent;
    boundaryEvent.kind = BoundaryEvent::Kind::TrackStart;
    boundaryEvent.url = newSource;
    boundaryEvent.sampleRate = format.sampleRate;
    boundaryEvent.channels = format.channels;
    boundaryEvent.duration = format.duration;
    const bool isLocal = DragonStreamFactory::isLocalSource(newSource);
    boundaryEvent.seekable = isLocal || (claimed && claimed->stream && claimed->stream->isSeekable());
    const bool handoff = !boundaryCallback || boundaryCallback(boundaryEvent);

    std::unique_ptr<Session> retired;
    bool promoted = false;
    lock.lock();
    if (handoff && m_phase == Phase::Promoting) {
        retired = std::exchange(m_activeSession, std::move(claimed));
        m_currentSource = newSource;
        m_phase = Phase::Starting;
        m_endReported = false;
        promoted = true;
    } else {
        retired = std::move(claimed);
        if (m_phase == Phase::Promoting) {
            m_phase = Phase::Finished;
        }
    }
    lock.unlock();

    // Stops the finished track's stream (closing its device, if any).
    retired.reset();

    if (!promoted) {
        qCDebug(dragonMediaBackendDecode) << "promotion of" << newSource.toString() << "discarded (rejected or superseded)";
        lock.lock();
        return false;
    }

    qCDebug(dragonMediaBackendDecode) << "gapless promotion to" << newSource.toString() << "sr=" << format.sampleRate << "ch=" << format.channels
                                      << "duration=" << format.duration.value_or(std::chrono::milliseconds{-1}).count() << "ms";

    // The action loop expects the lock held again on return.
    lock.lock();
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

    qCDebug(dragonMediaBackendDecode) << "initializeSession requesting decode session stop";
    m_sessionStopSource.request_stop();

    std::unique_ptr<Session> retired;
    {
        std::unique_lock lock(m_decoderMutex);
        qCDebug(dragonMediaBackendDecode) << "initializeSession waiting for decodeLoopActive=false (current=" << m_decodeLoopActive.load() << ")";
        m_stateChanged.wait(lock, [this]() {
            return !m_decodeLoopActive.load();
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
        m_startCompletion = completion;
        m_currentSource = source;
        m_activeSession = std::move(session);
        m_phase = Phase::Starting;
        m_endReported = false;
        m_sessionStopSource = std::stop_source{};
    }

    qCDebug(dragonMediaBackendDecode) << "initializeSession new session installed (phase=Starting), notifying";

    m_stateChanged.notify_all();

    co_return co_await *completion;
}

void DragonDecodePipeline::clearNextSource()
{
    cancelPreWarm();
    std::scoped_lock lock(m_decoderMutex);
    m_next.reset();
}

void DragonDecodePipeline::stopSession()
{
    {
        std::scoped_lock lock(m_decoderMutex);
        m_phase = Phase::Stopped;
    }

    bool stoppable = m_sessionStopSource.request_stop();
    qCDebug(dragonMediaBackendDecode) << "stopSession() called requesting decode session stop" << stoppable;

    {
        std::shared_ptr<DragonCompletion> pending;
        {
            std::scoped_lock lock(m_decoderMutex);
            pending = m_startCompletion;
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
        m_stateChanged.wait(lock, [this]() {
            return !m_decodeLoopActive.load();
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
            pending = m_startCompletion;
        }
        if (pending) {
            pending->cancel(QStringLiteral("Full stop"));
        }
    }

    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
    }

    m_stateChanged.notify_all();

    if (m_decodeThread.joinable()) {
        qCDebug(dragonMediaBackendDecode) << "stop() joining decode thread";
        m_decodeThread.join();
        qCDebug(dragonMediaBackendDecode) << "stop() decode thread joined";
    }
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.join();
    }

    std::unique_ptr<Session> activeSession;
    std::unique_ptr<Session> nextSession;
    {
        std::scoped_lock lock(m_decoderMutex);
        activeSession = std::move(m_activeSession);
        if (m_next) {
            nextSession = std::move(m_next->session);
            m_next.reset();
        }
        m_phase = Phase::Idle;
        m_decodeLoopActive = false;
    }
    activeSession.reset();
    nextSession.reset();
    qCDebug(dragonMediaBackendDecode) << "stop() full teardown complete";
}

void DragonDecodePipeline::setNextSource(const QUrl &next)
{
    cancelPreWarm();

    {
        std::scoped_lock lock(m_decoderMutex);
        m_next = NextTrack{};
        m_next->url = next;
    }

    if (next.isEmpty() || !DragonStreamFactory::supportsGaplessPreWarm(next)) {
        std::scoped_lock lock(m_decoderMutex);
        m_next.reset();
        return;
    }

    m_preWarmThread = std::jthread([this, next](std::stop_token st) {
        DragonThreadName::set("dragon-prewarm");
        auto session = createSession(next, true);
        if (st.stop_requested()) {
            return;
        }
        if (!session) {
            qCWarning(dragonMediaBackendDecode) << "pre-warm failed to create a session for" << next.toString();
            dropFailedPreWarm(next);
            return;
        }

        const InitResult result = session->decoder->initialize();
        if (st.stop_requested()) {
            qCDebug(dragonMediaBackendDecode) << "pre-warm of" << next.toString() << "cancelled";
            return;
        }
        if (!result.success) {
            qCDebug(dragonMediaBackendDecode) << "pre-warm of" << next.toString() << "failed to load";
            dropFailedPreWarm(next);
            return;
        }

        session->preInitialized = true;
        session->format = FormatReady{result.sampleRate, result.channels, result.duration};

        {
            std::scoped_lock lock(m_decoderMutex);
            if (!st.stop_requested() && m_next && m_next->url == next && !m_next->ready()) {
                m_next->session = std::move(session);
                m_next->format = *m_next->session->format;
                qCDebug(dragonMediaBackendDecode) << "pre-warm of" << next.toString() << "ready (sr=" << result.sampleRate << "ch=" << result.channels << ")";
            } else {
                qCDebug(dragonMediaBackendDecode) << "pre-warm of" << next.toString() << "superseded, discarding";
            }
        }

        m_stateChanged.notify_all();
    });
}

bool DragonDecodePipeline::isActive() const
{
    std::scoped_lock lock(m_decoderMutex);
    return m_activeSession && m_activeSession->decoder;
}

bool DragonDecodePipeline::hasFatalError() const
{
    std::scoped_lock lock(m_decoderMutex);
    return m_activeSession && m_activeSession->decoder && m_activeSession->decoder->hasFatalError();
}

void DragonDecodePipeline::requestSeek(std::chrono::milliseconds position)
{
    std::unique_lock lock(m_decoderMutex);
    // A seek must target the session that ends up current: a gapless
    // promotion may be in flight (recording its boundary), and seeking the
    // outgoing track's decoder would be a no-op (its decoder is already
    // destroyed at EOF). Wait for the promotion to resolve first; the wait
    // is bounded because the promotion's boundary callback is
    // non-blocking.
    m_stateChanged.wait(lock, [this]() {
        return m_phase != Phase::Promoting;
    });
    if (m_activeSession && m_activeSession->decoder) {
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

void DragonDecodePipeline::cancelPreWarm()
{
    if (m_preWarmThread.joinable()) {
        m_preWarmThread.request_stop();
        m_preWarmThread.join();
    }
    std::unique_ptr<Session> retired;
    {
        std::scoped_lock lock(m_decoderMutex);
        if (m_next) {
            retired = std::move(m_next->session);
            m_next->format.reset();
        }
    }
    retired.reset();
}

void DragonDecodePipeline::dropFailedPreWarm(const QUrl &next)
{
    {
        std::scoped_lock lock(m_decoderMutex);
        if (m_next && m_next->url == next) {
            m_next.reset();
        }
    }
    m_stateChanged.notify_all();
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

void DragonDecodePipeline::setBoundaryCallback(BoundaryCallback callback)
{
    std::scoped_lock lock(m_boundaryCallbackMutex);
    m_boundaryCallback = std::move(callback);
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
