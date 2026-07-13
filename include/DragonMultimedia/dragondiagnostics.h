/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmultimedia_export.h"

#include <QObject>

#include <cstddef>
#include <memory>

class DragonPlayer;

class DragonDiagnosticsPrivate;

class DRAGONMULTIMEDIA_EXPORT DragonDiagnostics : public QObject
{
public:
    explicit DragonDiagnostics(DragonPlayer *player);
    ~DragonDiagnostics() override;

    [[nodiscard]] int audioBufferUs() const;

    [[nodiscard]] int audioBufferFrames() const;

    [[nodiscard]] std::size_t decodeQueueSize() const;

    [[nodiscard]] std::size_t fftQueueSize() const;

    [[nodiscard]] bool decodeLoopActive() const;

    [[nodiscard]] bool hasActiveDecoder() const;

    [[nodiscard]] bool isAudioActive() const;

    [[nodiscard]] int audioUnderrunCount() const;

private:
    std::unique_ptr<DragonDiagnosticsPrivate> d;
};
