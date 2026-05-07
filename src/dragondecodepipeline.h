/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QUrl>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>

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

    void setCallbacks(FormatReadyCallback format,
                      DurationCallback duration,
                      SamplesCallback samples,
                      ErrorCallback error,
                      FinishedCallback finished,
                      GaplessTransitionCallback gapless);

    const std::unique_ptr<DragonDecoder> &activeDecoder() const;
    bool decodeLoopActive() const;

private:
    class Impl;
    std::unique_ptr<Impl> d;
};
