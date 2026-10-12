/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace DragonCdFrames
{

constexpr int kLeadInFrames = 150;
constexpr int kFramesPerSecond = 75;

[[nodiscard]] constexpr std::chrono::milliseconds framesToDuration(int64_t frames)
{
    if (frames <= 0) {
        return {};
    }
    return std::chrono::milliseconds(frames * 1000 / kFramesPerSecond);
}

}
