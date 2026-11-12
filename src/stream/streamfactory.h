/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "stream.h"

#include "dragonmediabackend_export.h"

#include <QUrl>

#include <memory>

class DRAGONMEDIABACKEND_EXPORT StreamFactory
{
public:
    static bool isLocalSource(const QUrl &url);

    static bool supportsGaplessPreWarm(const QUrl &url);

    static std::shared_ptr<Stream> createStream(const QUrl &source);
};
