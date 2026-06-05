/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"
#include <memory>

class DragonAudioSink;

DRAGONSDL_EXPORT std::unique_ptr<DragonAudioSink> createAudioSink();
