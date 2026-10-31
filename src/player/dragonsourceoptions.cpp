/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonsourceoptions.h"

#include <QSharedData>

#include <algorithm>

using namespace std::chrono_literals;

class DragonSourceOptionsPrivate : public QSharedData
{
public:
    std::chrono::milliseconds crossfadeDuration{0ms};
};

DragonSourceOptions::DragonSourceOptions() = default;
DragonSourceOptions::DragonSourceOptions(const DragonSourceOptions &other) = default;
DragonSourceOptions::DragonSourceOptions(DragonSourceOptions &&other) noexcept = default;
DragonSourceOptions::~DragonSourceOptions() = default;

DragonSourceOptions &DragonSourceOptions::operator=(const DragonSourceOptions &other) = default;
DragonSourceOptions &DragonSourceOptions::operator=(DragonSourceOptions &&other) noexcept = default;

bool DragonSourceOptions::operator==(const DragonSourceOptions &other) const
{
    if (d == other.d) {
        return true;
    }
    return crossfadeDuration() == other.crossfadeDuration();
}

bool DragonSourceOptions::operator!=(const DragonSourceOptions &other) const
{
    return !(*this == other);
}

std::chrono::milliseconds DragonSourceOptions::crossfadeDuration() const
{
    return d ? d->crossfadeDuration : 0ms;
}

void DragonSourceOptions::setCrossfadeDuration(std::chrono::milliseconds duration)
{
    if (!d) {
        d = new DragonSourceOptionsPrivate;
    }
    d->crossfadeDuration = std::max(0ms, duration);
}
