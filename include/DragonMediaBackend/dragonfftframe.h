/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QMetaType>
#include <QSharedDataPointer>

#include <chrono>
#include <cstddef>
#include <span>

class DragonFftFramePrivate;

class DRAGONMEDIABACKEND_EXPORT DragonFftFrame
{
public:
    static constexpr std::size_t NUM_FREQUENCIES = 512;
    static constexpr std::size_t NUM_BARS = 24;

    DragonFftFrame();
    DragonFftFrame(std::span<const float> frequenciesDb, std::span<const float> barData, std::chrono::microseconds timestamp);
    DragonFftFrame(const DragonFftFrame &other);
    DragonFftFrame &operator=(const DragonFftFrame &other);
    DragonFftFrame(DragonFftFrame &&other) noexcept;
    DragonFftFrame &operator=(DragonFftFrame &&other) noexcept;
    ~DragonFftFrame();

    [[nodiscard]] std::span<const float> frequencies() const;
    [[nodiscard]] std::span<const float> bars() const;
    [[nodiscard]] std::chrono::microseconds timestamp() const;

    [[nodiscard]] bool operator==(const DragonFftFrame &other) const;
    [[nodiscard]] bool operator!=(const DragonFftFrame &other) const;

private:
    QSharedDataPointer<DragonFftFramePrivate> d;
};

Q_DECLARE_METATYPE(DragonFftFrame)
