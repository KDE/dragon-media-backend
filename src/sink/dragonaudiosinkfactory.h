/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include <DragonMediaBackend/dragonaudiooutput.h>
#include <memory>

class DragonAudioSink;

DRAGONMEDIABACKEND_EXPORT std::unique_ptr<DragonAudioSink> createAudioSink(DragonAudioOutput::Backend requestedSink = DragonAudioOutput::Backend::Auto,
                                                                           DragonAudioOutput::Backend *selectedSinkOut = nullptr);
