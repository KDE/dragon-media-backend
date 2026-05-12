/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

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
class DragonRadioStream;

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

    void setSource(const QUrl &source, uint64_t generation);
    void setNextSource(const QUrl &next, uint64_t generation);

    void stopSession();

    void stop();

    bool isActive() const;
    bool hasFatalError() const;
    uint64_t generation() const;

    void requestSeek(int64_t posMs);

    using SamplesCallback = std::function<void(std::span<const std::float32_t> samples, const std::stop_token &st)>;

    void setSamplesCallback(SamplesCallback callback);

    const std::unique_ptr<DragonDecoder> &activeDecoder() const;
    bool decodeLoopActive() const;

Q_SIGNALS:
    void formatReady(int sampleRate, int channels, bool isGapless);
    void durationChanged(int64_t durationMs);
    void errorOccurred(const QString &message);
    void finished(bool hadFatalError);
    void gaplessTransition(const QUrl &newSource);

private:
    std::unique_ptr<DragonDecoder> createDecoder(const QUrl &source, bool isGapless, uint64_t generation);

    void startDecodeThread();
    bool waitForDecoderAssignment(std::stop_token st);
    std::pair<bool, bool> executeDecodeSession();
    void processDecodeCompletion();

    SamplesCallback m_samplesCallback;

    DragonPlayer *m_player = nullptr;
    uint64_t m_generation = 0;
    QUrl m_nextSource;

    std::jthread m_decodeThread;
    std::stop_source m_sessionStopSource;
    mutable std::mutex m_decoderMutex;
    std::condition_variable_any m_decoderCv;
    bool m_decodeLoopActive = false;
    std::unique_ptr<DragonDecoder> m_activeDecoder;
    std::unique_ptr<DragonDecoder> m_preWarmedDecoder;

    std::jthread m_preWarmThread;

    std::unique_ptr<DragonRadioStream> m_radioStream;
};
