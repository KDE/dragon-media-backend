/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <QObject>

#include <atomic>
#include <cstddef>
#include <cstdint>

class DragonPlayer;

class DRAGONSDL_EXPORT DragonDiagnostics : public QObject
{
public:
    explicit DragonDiagnostics(DragonPlayer *player);

    [[nodiscard]] int sdlAudioBufferMs() const;

    [[nodiscard]] int sdlAudioBufferUs() const;

    [[nodiscard]] int sdlAudioBufferFrames() const;

    [[nodiscard]] std::size_t decodeQueueSize() const;

    [[nodiscard]] std::size_t fftQueueSize() const;

    [[nodiscard]] bool decodeLoopActive() const;

    [[nodiscard]] bool hasActiveDecoder() const;

    [[nodiscard]] float audioCallbackHz() const;

private:
    DragonPlayer *m_player;

    mutable std::atomic<uint64_t> m_callbackCount{0};
    mutable std::atomic<uint64_t> m_callbackTimestampUs{0};
    mutable std::atomic<float> m_callbackHz{0.0f};
};
