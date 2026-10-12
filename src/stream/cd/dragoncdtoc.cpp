/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragoncdtoc.h>

#include "dragoncdframes.h"
#include "dragoncdtoc_p.h"

#include <utility>

DragonCdToc::DragonCdToc()
    : d(new DragonCdTocPrivate)
{
}

DragonCdToc::DragonCdToc(DragonCdTocPrivate *tocPrivate)
    : d(tocPrivate)
{
}

DragonCdToc::~DragonCdToc() = default;
DragonCdToc::DragonCdToc(const DragonCdToc &) = default;
DragonCdToc &DragonCdToc::operator=(const DragonCdToc &) = default;

DragonCdToc::DragonCdToc(DragonCdToc &&other) noexcept
    : d(std::move(other.d))
{
    // Leave the source usable, as a default-constructed placeholder.
    other.d = new DragonCdTocPrivate;
}

DragonCdToc &DragonCdToc::operator=(DragonCdToc &&other) noexcept
{
    if (this != &other) {
        d = std::move(other.d);
        other.d = new DragonCdTocPrivate;
    }
    return *this;
}

bool DragonCdToc::operator==(const DragonCdToc &other) const
{
    return d == other.d || (d->device == other.d->device && d->leadOutFrame == other.d->leadOutFrame && d->tracks == other.d->tracks);
}

QString DragonCdToc::device() const
{
    return d->device;
}

QList<DragonCdTrack> DragonCdToc::tracks() const
{
    return d->tracks;
}

int DragonCdToc::leadOutFrame() const
{
    return d->leadOutFrame;
}

std::chrono::milliseconds DragonCdToc::discDuration() const
{
    return DragonCdFrames::framesToDuration(qint64(d->leadOutFrame) - DragonCdFrames::kLeadInFrames);
}

QList<quint32> DragonCdToc::cddbTrackOffsetList() const
{
    if (d->tracks.isEmpty() && d->leadOutFrame <= 0) {
        return {};
    }
    QList<quint32> offsets;
    offsets.reserve(d->tracks.size() + 1);
    for (const DragonCdTrack &track : d->tracks) {
        offsets.append(quint32(track.startFrame()));
    }
    offsets.append(quint32(d->leadOutFrame));
    return offsets;
}
