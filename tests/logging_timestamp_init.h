/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Include this header in test executables to enable timestamped logging.
 * Uses static initialization to install the handler before main() runs.
 */

#include "logging_timestamp.h"

static struct InstallTimestampedHandler {
    InstallTimestampedHandler()
    {
        dragonsdl_install_timestamped_handler();
    }
} installTimestampedHandlerInstance;