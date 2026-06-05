/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiosinkfactory.h"
#include "dragonaudiosink.h"
#include "dragonsdlaudiosink.h"

std::unique_ptr<DragonAudioSink> createAudioSink()
{
    if (DragonSdlAudioSink::isAvailable()) {
        return std::make_unique<DragonSdlAudioSink>();
    }

    return nullptr;
}
