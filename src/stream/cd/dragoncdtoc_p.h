/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <dragoncdtoc.h>

#include <QSharedData>
#include <QUrl>

class QString;

class DragonCdTrackPrivate : public QSharedData
{
public:
    int number = 0;
    bool isAudio = false;
    int startFrame = 0;
    int lengthFrames = 0;
    QUrl url;
};

class DragonCdTocPrivate : public QSharedData
{
public:
    QString device;
    int leadOutFrame = 0;
    QList<DragonCdTrack> tracks;
};
