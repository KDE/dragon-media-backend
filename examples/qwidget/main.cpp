/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "mainwindow.h"

#include <QApplication>
#include <QLoggingCategory>

using namespace Qt::StringLiterals;

static void enableDebugOutput()
{
    QLoggingCategory::setFilterRules(
        QStringLiteral("org.kde.dragonsdl.*.debug=true\n"
                       "qt.qpa.*=false\n"
                       "qt.network.*=false\n"
                       "qt.dbus*=false\n"
                       "qt.svg*=false"));
}

struct EnableDebugOutput {
    EnableDebugOutput()
    {
        enableDebugOutput();
    }
};

static const EnableDebugOutput enableDebugOutputInstance;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setWindowIcon(QIcon::fromTheme("emblem-music-symbolic"_L1));
    QApplication::setOrganizationName("DragonSDL"_L1);
    QApplication::setApplicationName("Dragon SDL Example"_L1);

    MainWindow w;
    w.show();
    return QApplication::exec();
}
