/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <cstddef>

class DragonPlayer;

class DRAGONSDL_EXPORT DragonDiagnostics
{
public:
    explicit DragonDiagnostics(DragonPlayer &player);

    [[nodiscard]] int sdlAudioBufferMs() const;

    [[nodiscard]] std::size_t decodeQueueSize() const;

    [[nodiscard]] std::size_t fftQueueSize() const;

private:
    DragonPlayer &m_player;
};
