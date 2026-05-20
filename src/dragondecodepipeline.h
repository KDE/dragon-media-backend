/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragoncompletion.h"
#include "dragonevent.h"

#include <QCoroTask>
#include <QObject>
#include <QUrl>
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

    QCoro::Task<DragonSdl::InitResult> initializeSession(QUrl source, bool isGapless = false);
    void setNextSource(const QUrl &next);

    void stopSession();

    void stop();
    bool isActive() const;
    bool hasFatalError() const;

    void requestSeek(int64_t posMs);

    using SamplesCallback = std::function<void(std::span<const std::float32_t> samples, const std::stop_token &st)>;

    void setSamplesCallback(SamplesCallback callback);

    static DragonSdl::InitResult makeSuccessResult(const DragonSdl::FormatReady &fr, bool isGapless = false);
    static DragonSdl::InitResult makeErrorResult(const QString &message, bool isGapless = false);
    static DragonSdl::InitResult makeCancelledResult(const QString &message, bool isGapless = false);

    const std::unique_ptr<DragonDecoder> &activeDecoder() const;
    bool decodeLoopActive() const;

    void setCurrentSource(const QUrl &source);
Q_SIGNALS:

    void sessionError(const QString &message);

    void sessionFinished(const QUrl &source, bool hadFatalError);

    void gaplessTransition(const QUrl &newSource, int sampleRate, int channels, qint64 durationMs);

    void bufferProgressChanged(double progress);

private:
    std::unique_ptr<DragonDecoder> createDecoder(const QUrl &source, bool isGapless);
    void cancelPreWarm(const QString &reason);

    void startDecodeThread();
    bool waitForDecoderAssignment(std::stop_token st);
    std::pair<bool, bool> executeDecodeSession();
    void processDecodeCompletion();

    SamplesCallback m_samplesCallback;

    DragonPlayer *m_player = nullptr;
    QUrl m_currentSource;
    QUrl m_nextSource;

    std::jthread m_decodeThread;
    std::stop_source m_sessionStopSource;
    mutable std::mutex m_decoderMutex;
    std::condition_variable_any m_decoderAssignedCv;
    std::condition_variable_any m_decodeLoopFinishedCv;
    bool m_decodeLoopActive = false;
    std::unique_ptr<DragonDecoder> m_activeDecoder;
    std::unique_ptr<DragonDecoder> m_preWarmedDecoder;

    std::jthread m_preWarmThread;

    std::shared_ptr<DragonSdl::DragonCompletion> m_pendingInitCompletion;

    std::shared_ptr<DragonSdl::DragonCompletion> m_pendingGaplessCompletion;

    std::unique_ptr<DragonStream> m_stream;
};
