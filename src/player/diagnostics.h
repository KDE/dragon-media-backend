/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

class DragonPlayer;
class DragonSpectrumAnalyzer;
class PlaybackPipeline;

class DiagnosticsPrivate;

class DRAGONMEDIABACKEND_EXPORT Diagnostics : public QObject
{
    Q_OBJECT
public:
    explicit Diagnostics(DragonPlayer *player);
    ~Diagnostics() override;

    void setSpectrumAnalyzer(DragonSpectrumAnalyzer *analyzer);

    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const;

    [[nodiscard]] int audioBufferFrames() const;

    [[nodiscard]] std::size_t decodeQueueSize() const;

    [[nodiscard]] qint64 audioSamplesRead() const;

    [[nodiscard]] std::size_t fftQueueSize() const;

    [[nodiscard]] bool decodeLoopActive() const;

    [[nodiscard]] bool hasActiveDecoder() const;

    [[nodiscard]] bool isAudioActive() const;

    [[nodiscard]] int audioUnderrunCount() const;

    [[nodiscard]] std::chrono::milliseconds audioPosition() const;

    [[nodiscard]] bool isQueueReady() const;

    [[nodiscard]] bool isFlushPending() const;

    [[nodiscard]] qint64 producerSamples() const;

    [[nodiscard]] std::uint64_t streamEpoch() const;

    [[nodiscard]] std::uint64_t primarySlotStreamEpoch() const;

    [[nodiscard]] PlaybackPipeline *playbackPipeline() const;

private:
    std::unique_ptr<DiagnosticsPrivate> d;
};
