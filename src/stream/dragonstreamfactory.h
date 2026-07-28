/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonstream.h"

#include <QUrl>

#include <memory>

class DragonStreamFactory
{
public:
    static std::shared_ptr<DragonStream> createStream(const QUrl &source);
};
