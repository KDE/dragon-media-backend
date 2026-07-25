/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstdfloat_compat.h"
#include <array>
#include <chrono>
#include <cstddef>

struct DragonFftBlock {
    static constexpr size_t MAX_SAMPLES = 1024;
    std::array<std::float32_t, MAX_SAMPLES> samples;
    size_t count = 0;
    std::chrono::microseconds pts;
};
