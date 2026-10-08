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

    void clearNextSource();

    void stopSession();

    void stop();
    bool isActive() const;
    bool hasFatalError() const;

    void requestSeek(std::chrono::milliseconds position);

    using SamplesCallback = std::function<void(std::span<const float> samples, const std::stop_token &st)>;

    void setSamplesCallback(SamplesCallback callback);

    struct BoundaryEvent {
        enum class Kind {
            TrackStart,
            EndOfStream,
        };
        Kind kind;
        QUrl url; // TrackStart only
        int sampleRate = 0; // TrackStart only
        int channels = 0; // TrackStart only
        std::optional<std::chrono::milliseconds> duration; // TrackStart only
    };
    using BoundaryCallback = std::function<bool(const BoundaryEvent &)>;
    void setBoundaryCallback(BoundaryCallback callback);

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
        bool preInitialized = false;
        std::optional<DragonMediaBackend::FormatReady> format;
    };

    struct NextTrack {
        QUrl url;
        std::unique_ptr<Session> session;
        std::optional<DragonMediaBackend::FormatReady> format;
        [[nodiscard]] bool ready() const
        {
            return session && format;
        }
    };

    enum class Phase {
        Idle,
        Starting,
        Decoding,
        Finished,
        Stopped,
    };

    std::unique_ptr<Session> createSession(const QUrl &source, bool isPreWarm);
    void cancelPreWarm();

    void dropFailedPreWarm(const QUrl &next);

    enum class Action {
        Wait,
        Decode,
        Promote,
        Finalize,
    };
    [[nodiscard]] Action nextAction() const;
    void decodeThreadMain(std::stop_token st);
    void runCurrentSession(std::unique_lock<std::mutex> &lock);
    bool promoteNext(std::unique_lock<std::mutex> &lock);
    void finalizeSession(std::unique_lock<std::mutex> &lock);
    void startDecodeThread();

    SamplesCallback m_samplesCallback;
    // Guards m_samplesCallback, which is installed from the main thread and
    // invoked on the decode thread.
    mutable std::mutex m_samplesCallbackMutex;

    BoundaryCallback m_boundaryCallback;
    mutable std::mutex m_boundaryCallbackMutex;

    DragonPlayer *m_player = nullptr;

    std::jthread m_decodeThread;
    std::stop_source m_sessionStopSource;
    mutable std::mutex m_decoderMutex;
    std::condition_variable_any m_stateChanged;
    // Written on the decode thread, read from the main thread (diagnostics).
    std::atomic<bool> m_decodeLoopActive{false};

    Phase m_phase = Phase::Idle;
    QUrl m_currentSource;
    std::unique_ptr<Session> m_activeSession;
    std::optional<NextTrack> m_next;
    bool m_endReported = false;
    std::shared_ptr<DragonMediaBackend::DragonCompletion> m_startCompletion;

    std::jthread m_preWarmThread;
};
