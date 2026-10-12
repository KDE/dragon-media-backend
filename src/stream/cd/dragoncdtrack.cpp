/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <dragoncdtrack.h>

#include "dragoncdframes.h"
#include "dragoncdtoc_p.h"

#include <utility>

DragonCdTrack::DragonCdTrack()
    : d(new DragonCdTrackPrivate)
{
}

DragonCdTrack::DragonCdTrack(DragonCdTrackPrivate *trackPrivate)
    : d(trackPrivate)
{
}

DragonCdTrack::~DragonCdTrack() = default;
DragonCdTrack::DragonCdTrack(const DragonCdTrack &) = default;
DragonCdTrack &DragonCdTrack::operator=(const DragonCdTrack &) = default;

DragonCdTrack::DragonCdTrack(DragonCdTrack &&other) noexcept
    : d(std::move(other.d))
{
    // Leave the source usable, as a default-constructed placeholder.
    other.d = new DragonCdTrackPrivate;
}

DragonCdTrack &DragonCdTrack::operator=(DragonCdTrack &&other) noexcept
{
    if (this != &other) {
        d = std::move(other.d);
        other.d = new DragonCdTrackPrivate;
    }
    return *this;
}

bool DragonCdTrack::operator==(const DragonCdTrack &other) const
{
    return d == other.d
        || (d->number == other.d->number && d->isAudio == other.d->isAudio && d->startFrame == other.d->startFrame && d->lengthFrames == other.d->lengthFrames
            && d->url == other.d->url);
}

int DragonCdTrack::number() const
{
    return d->number;
}

bool DragonCdTrack::isAudio() const
{
    return d->isAudio;
}

int DragonCdTrack::startFrame() const
{
    return d->startFrame;
}

int DragonCdTrack::lengthFrames() const
{
    return d->lengthFrames;
}

std::chrono::milliseconds DragonCdTrack::duration() const
{
    return DragonCdFrames::framesToDuration(d->lengthFrames);
}

QUrl DragonCdTrack::url() const
{
    return d->url;
}
