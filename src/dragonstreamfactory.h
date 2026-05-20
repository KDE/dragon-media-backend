/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonstream.h"

#include <QUrl>

#include <memory>

class DragonStreamFactory
{
public:
    static std::unique_ptr<DragonStream> createStream(const QUrl &source);
};