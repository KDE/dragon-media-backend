/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Shared timestamped logging utilities for tests and examples.
 * This does NOT affect logging when DragonMultimedia is used as a library.
 */

#pragma once

#include <QDateTime>
#include <QLoggingCategory>
#include <QMessageLogContext>

#include <cstdio>

inline void DragonMultimedia_install_timestamped_handler()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &msg) {
        const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"));

        QString formattedMsg;
        switch (type) {
        case QtDebugMsg:
            formattedMsg = QStringLiteral("[%1] %2").arg(timestamp, msg);
            break;
        case QtInfoMsg:
            formattedMsg = QStringLiteral("[%1] %2").arg(timestamp, msg);
            break;
        case QtWarningMsg:
            formattedMsg = QStringLiteral("[%1] WARNING: %2").arg(timestamp, msg);
            break;
        case QtCriticalMsg:
            formattedMsg = QStringLiteral("[%1] CRITICAL: %2").arg(timestamp, msg);
            break;
        case QtFatalMsg:
            formattedMsg = QStringLiteral("[%1] FATAL: %2").arg(timestamp, msg);
            break;
        }

        if (context.category) {
            QLoggingCategory category(context.category);
            if (!category.isEnabled(type)) {
                return;
            }
        }

        fprintf(stderr, "%s\n", qUtf8Printable(formattedMsg));
    });
}