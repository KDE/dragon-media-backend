/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonmultimedia_export.h"
#include <memory>

class DragonAudioSink;

DRAGONMULTIMEDIA_EXPORT std::unique_ptr<DragonAudioSink> createAudioSink();
