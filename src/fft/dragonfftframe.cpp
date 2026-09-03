/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <DragonMediaBackend/dragonfftframe.h>

#include <QSharedData>

#include <algorithm>
#include <array>

class DragonFftFramePrivate : public QSharedData
{
public:
    std::array<float, DragonFftFrame::NUM_FREQUENCIES> frequenciesDb{};
    std::array<float, DragonFftFrame::NUM_BARS> barData{};
    std::chrono::microseconds timestamp{};
};

DragonFftFrame::DragonFftFrame()
    : d(new DragonFftFramePrivate)
{
}

DragonFftFrame::DragonFftFrame(std::span<const float> frequenciesDb, std::span<const float> barData, std::chrono::microseconds timestamp)
    : d(new DragonFftFramePrivate)
{
    const auto frequencyCount = std::min(frequenciesDb.size(), d->frequenciesDb.size());
    std::copy_n(frequenciesDb.begin(), frequencyCount, d->frequenciesDb.begin());
    const auto barCount = std::min(barData.size(), d->barData.size());
    std::copy_n(barData.begin(), barCount, d->barData.begin());
    d->timestamp = timestamp;
}

DragonFftFrame::DragonFftFrame(const DragonFftFrame &other) = default;
DragonFftFrame &DragonFftFrame::operator=(const DragonFftFrame &other) = default;
DragonFftFrame::DragonFftFrame(DragonFftFrame &&other) noexcept = default;
DragonFftFrame &DragonFftFrame::operator=(DragonFftFrame &&other) noexcept = default;
DragonFftFrame::~DragonFftFrame() = default;

std::span<const float> DragonFftFrame::frequencies() const
{
    return d->frequenciesDb;
}

std::span<const float> DragonFftFrame::bars() const
{
    return d->barData;
}

std::chrono::microseconds DragonFftFrame::timestamp() const
{
    return d->timestamp;
}

bool DragonFftFrame::operator==(const DragonFftFrame &other) const
{
    if (d == other.d) {
        return true;
    }
    return d->timestamp == other.d->timestamp && d->frequenciesDb == other.d->frequenciesDb && d->barData == other.d->barData;
}

bool DragonFftFrame::operator!=(const DragonFftFrame &other) const
{
    return !(*this == other);
}
