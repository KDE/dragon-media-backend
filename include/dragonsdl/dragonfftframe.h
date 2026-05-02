/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <vector>

struct DragonFftFrame {
    std::vector<float> frequenciesDb;

    std::vector<float> barData;

    std::chrono::microseconds timestamp{};
};