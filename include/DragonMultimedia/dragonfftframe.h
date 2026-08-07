/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <array>
#include <chrono>

#include <QMetaType>

struct DragonFftFrame {
    static constexpr size_t NUM_FREQUENCIES = 512;
    static constexpr size_t NUM_BARS = 24;

    std::array<float, NUM_FREQUENCIES> frequenciesDb{};

    std::array<float, NUM_BARS> barData{};

    std::chrono::microseconds timestamp{};
};

Q_DECLARE_METATYPE(DragonFftFrame)
