/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Include this header in test executables to enable timestamped logging.
 * Uses static initialization to install the handler before main() runs.
 */

#include "stream/logging_timestamp.h"

static struct InstallTimestampedHandler {
    InstallTimestampedHandler()
    {
        DragonMediaBackend_install_timestamped_handler();
    }
} installTimestampedHandlerInstance;