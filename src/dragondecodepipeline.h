/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

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
class DragonRadioStream;

class DragonDecodePipeline
{
public:
    explicit DragonDecodePipeline(DragonPlayer *player);
    ~DragonDecodePipeline();

    DragonDecodePipeline(const DragonDecodePipeline &) = delete;
    DragonDecodePipeline &operator=(const DragonDecodePipeline &) = delete;
    DragonDecodePipeline(DragonDecodePipeline &&) = delete;
    DragonDecodePipeline &operator=(DragonDecodePipeline &&) = delete;

    void setSource(const QUrl &source, uint64_t generation);
    void setNextSource(const QUrl &next, uint64_t generation);

    void stopSession();

    void stop();

    bool isActive() const;
    bool hasFatalError() const;
    uint64_t generation() const;

    void requestSeek(int64_t posMs);

    using FormatReadyCallback = std::function<void(int sampleRate, int channels, bool isGapless)>;
    using DurationCallback = std::function<void(int64_t durationMs)>;
    using SamplesCallback = std::function<void(std::span<const std::float32_t> samples, const std::stop_token &st)>;
    using ErrorCallback = std::function<void(const QString &message)>;
    using FinishedCallback = std::function<void(bool hadFatalError, bool wasStopped)>;
    using GaplessTransitionCallback = std::function<void(const QUrl &newSource)>;

    void setFormatReadyCallback(FormatReadyCallback callback);
    void setDurationCallback(DurationCallback callback);
    void setSamplesCallback(SamplesCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setFinishedCallback(FinishedCallback callback);
    void setGaplessTransitionCallback(GaplessTransitionCallback callback);

    const std::unique_ptr<DragonDecoder> &activeDecoder() const;
    bool decodeLoopActive() const;

private:
    void startDecodeThread();
    std::unique_ptr<DragonDecoder> createDecoder(const QUrl &source, bool isGapless, uint64_t generation);

    FormatReadyCallback m_formatReadyCallback;
    DurationCallback m_durationCallback;
    SamplesCallback m_samplesCallback;
    ErrorCallback m_errorCallback;
    FinishedCallback m_finishedCallback;
    GaplessTransitionCallback m_gaplessTransitionCallback;

    DragonPlayer *q = nullptr;
    uint64_t m_generation = 0;
    QUrl m_nextSource;

    std::jthread m_decodeThread;
    std::stop_source m_decodeStopSource;
    mutable std::mutex m_decoderMutex;
    std::condition_variable m_decoderCv;
    bool m_decodeLoopActive = false;
    std::unique_ptr<DragonDecoder> m_activeDecoder;
    std::unique_ptr<DragonDecoder> m_preWarmedDecoder;

    std::jthread m_preWarmThread;

    std::unique_ptr<DragonRadioStream> m_radioStream;
};
