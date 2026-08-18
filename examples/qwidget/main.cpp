/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "mainwindow.h"
#include "stream/logging_timestamp.h"

#include <QApplication>
#include <QLoggingCategory>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

static void enableDebugOutput()
{
    QLoggingCategory::setFilterRules(
        QStringLiteral("org.kde.dragonmediabackend.*.debug=true\n"
                       "qt.qpa.*=false\n"
                       "qt.network.*=false\n"
                       "qt.dbus*=false\n"
                       "qt.svg*=false"));
}

struct EnableDebugOutput {
    EnableDebugOutput()
    {
        DragonMediaBackend_install_timestamped_handler();
        enableDebugOutput();
    }
};

static const EnableDebugOutput enableDebugOutputInstance;

int main(int argc, char *argv[])
{
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("DragonMediaBackend-qwidget-example"));

    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon::fromTheme("emblem-music-symbolic"_L1));
    QApplication::setOrganizationName("DragonMediaBackend"_L1);
    QApplication::setApplicationName("Dragon SDL Example"_L1);

    MainWindow w;
    w.show();
    return QApplication::exec();
}
