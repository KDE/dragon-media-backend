/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <stdfloat>

struct DragonFftBlock {
    static constexpr size_t MAX_SAMPLES = 1024;
    std::array<std::float32_t, MAX_SAMPLES> samples;
    size_t count = 0;
    std::chrono::microseconds pts;
};
