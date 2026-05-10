/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <QObject>

#include <memory>

class DragonPlayer;

class DragonDiagnosticsPrivate;

class DRAGONSDL_EXPORT DragonDiagnostics : public QObject
{
public:
    explicit DragonDiagnostics(DragonPlayer *player);
    ~DragonDiagnostics() override;

    [[nodiscard]] int sdlAudioBufferUs() const;

    [[nodiscard]] int sdlAudioBufferFrames() const;

    [[nodiscard]] std::size_t decodeQueueSize() const;

    [[nodiscard]] std::size_t fftQueueSize() const;

    [[nodiscard]] bool decodeLoopActive() const;

    [[nodiscard]] bool hasActiveDecoder() const;

    [[nodiscard]] float audioCallbackHz() const;

private:
    std::unique_ptr<DragonDiagnosticsPrivate> d;
};
