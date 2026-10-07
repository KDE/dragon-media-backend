/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragoncompletion.h"
#include "player/dragonevent.h"

#include <QCoroTask>
#include <QObject>
#include <QUrl>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>

class DragonPlayer;
class DragonDecoder;
class DragonStream;

class DragonDecodePipeline : public QObject
{
    Q_OBJECT
public:
    explicit DragonDecodePipeline(DragonPlayer *player);
    ~DragonDecodePipeline();

    DragonDecodePipeline(const DragonDecodePipeline &) = delete;
    DragonDecodePipeline &operator=(const DragonDecodePipeline &) = delete;
    DragonDecodePipeline(DragonDecodePipeline &&) = delete;
    DragonDecodePipeline &operator=(DragonDecodePipeline &&) = delete;

    QCoro::Task<DragonMediaBackend::InitResult> initializeSession(QUrl source, bool isGapless = false);
    void setNextSource(const QUrl &next);

    void stopSession();

    void stop();
    bool isActive() const;
    bool hasFatalError() const;

    void requestSeek(std::chrono::milliseconds position);

    using SamplesCallback = std::function<void(std::span<const float> samples, const std::stop_token &st)>;

    void setSamplesCallback(SamplesCallback callback);

    static DragonMediaBackend::InitResult makeSuccessResult(const DragonMediaBackend::FormatReady &fr, bool isGapless = false);
    static DragonMediaBackend::InitResult makeErrorResult(const QString &message, bool isGapless = false);
    static DragonMediaBackend::InitResult makeCancelledResult(const QString &message, bool isGapless = false);

    bool decodeLoopActive() const;

    void setCurrentSource(const QUrl &source);

    qint64 streamSize() const;

    bool streamIsSeekable() const;

Q_SIGNALS:
    void sessionError(const QString &message);

    void sessionFinished(const QUrl &source, bool hadFatalError);

    void gaplessTransition(const QUrl &newSource, int sampleRate, int channels, std::optional<std::chrono::milliseconds> duration);

    void bufferProgressChanged(qreal progress);

    void streamStalled();
    void streamBuffering();
    void streamBuffered();

    void streamSeekableChanged(bool seekable);

private:
    struct Session {
        Session(DragonDecodePipeline *owner, std::shared_ptr<DragonStream> stream);
        ~Session();
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        DragonDecodePipeline *const owner;
        const std::shared_ptr<DragonStream> stream;
        std::unique_ptr<DragonDecoder> decoder;
    };

    std::unique_ptr<Session> createSession(const QUrl &source, bool isPreWarm);
    void cancelPreWarm(const QString &reason);
    // Requires m_decoderMutex.
    bool hasActiveDecoder() const;
    // Requires m_decoderMutex.
    bool hasLateHandoff() const;

    bool promotePreWarmedDecoder();

    void startDecodeThread();
    bool waitForDecoderAssignment(std::stop_token st);
    std::pair<bool, bool> executeDecodeSession();
    void processDecodeCompletion();

    SamplesCallback m_samplesCallback;
    // Guards m_samplesCallback, which is installed from the main thread and
    // invoked on the decode thread.
    mutable std::mutex m_samplesCallbackMutex;

    DragonPlayer *m_player = nullptr;
    QUrl m_currentSource;
    QUrl m_nextSource;

    std::jthread m_decodeThread;
    std::stop_source m_sessionStopSource;
    mutable std::mutex m_decoderMutex;
    std::condition_variable_any m_decoderAssignedCv;
    std::condition_variable_any m_decodeLoopFinishedCv;
    // Written on the decode thread, read from the main thread (diagnostics).
    std::atomic<bool> m_decodeLoopActive{false};
    std::unique_ptr<Session> m_activeSession;
    // The next track, decoded up to its first frame for a gapless handoff.
    std::unique_ptr<Session> m_preWarmedSession;
    bool m_awaitingLateHandoff = false;

    std::jthread m_preWarmThread;

    std::shared_ptr<DragonMediaBackend::DragonCompletion> m_pendingInitCompletion;

    std::shared_ptr<DragonMediaBackend::DragonCompletion> m_pendingGaplessCompletion;
};
