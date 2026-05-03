/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "mainwindow.h"

#include <QApplication>

using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("DragonSDL"_L1);
    app.setApplicationName("DragonSDLExample"_L1);

    MainWindow w;
    w.show();
    return app.exec();
}
