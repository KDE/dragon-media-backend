// SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "dragonmediabackend_audio_logging.h"

const QLoggingCategory &dragonMediaBackendAudio()
{
    static const QLoggingCategory category("org.kde.dragonmediabackend.audio", QtInfoMsg);
    return category;
}
