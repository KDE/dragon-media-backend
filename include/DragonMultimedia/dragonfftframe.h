/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <chrono>
#include <vector>

#include <QMetaType>

struct DragonFftFrame {
    std::vector<float> frequenciesDb;

    std::vector<float> barData;

    std::chrono::microseconds timestamp{};
};

Q_DECLARE_METATYPE(DragonFftFrame)