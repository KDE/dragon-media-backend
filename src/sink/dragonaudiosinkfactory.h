/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmultimedia_export.h"
#include <DragonMultimedia/dragonplayer.h>
#include <memory>

class DragonAudioSink;

DRAGONMULTIMEDIA_EXPORT std::unique_ptr<DragonAudioSink> createAudioSink(DragonPlayer::AudioSink requestedSink = DragonPlayer::AudioSink::Auto,
                                                                         DragonPlayer::AudioSink *selectedSinkOut = nullptr);
