/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QString>
#include <QUrl>

#include <optional>

namespace DragonCdUrl
{

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QUrl makeTrackUrl(int trackNumber, const QString &device);
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QString deviceFromUrl(const QUrl &url);
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT std::optional<int> trackNumberFromName(const QString &fileName, int firstTrackNum, int lastTrackNum);
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT std::optional<int> resolveTrackNumber(const QUrl &url, int firstTrackNum, int lastTrackNum);

}
