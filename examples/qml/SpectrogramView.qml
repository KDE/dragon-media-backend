/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property VisualizationController visualization

    clip: true

    WaterfallItem {
        anchors.fill: parent
        visualization: root.visualization
    }
}
