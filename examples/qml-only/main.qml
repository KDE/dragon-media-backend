/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * QML-only example: no C++ code, no context properties, no imperative type
 * registration. Run it with the stock QML runtime from the build tree:
 *
 *   QT_PLUGIN_PATH=build/bin qml6 -I build/bin examples/qml-only/main.qml
 *
 * or, from an installed prefix (with the audio sink plugins on the Qt
 * plugin path):
 *
 *   QT_PLUGIN_PATH=<prefix>/lib64/plugins qml6 -I <prefix>/lib64/qml \
 *       examples/qml-only/main.qml
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import org.kde.dragonmediabackend

ApplicationWindow {
    id: root

    width: 520
    height: 240
    visible: true
    title: qsTr("dragon-media-backend QML-only example")

    DragonPlayer {
        id: player
    }

    function stateName(state) {
        switch (state) {
        case DragonPlayer.PlayingState:
            return qsTr("Playing");
        case DragonPlayer.PausedState:
            return qsTr("Paused");
        default:
            return qsTr("Stopped");
        }
    }

    function statusName(status) {
        switch (status) {
        case DragonPlayer.NoMedia:
            return qsTr("No media");
        case DragonPlayer.LoadingMedia:
            return qsTr("Loading");
        case DragonPlayer.LoadedMedia:
            return qsTr("Loaded");
        case DragonPlayer.BufferingMedia:
            return qsTr("Buffering");
        case DragonPlayer.StalledMedia:
            return qsTr("Stalled");
        case DragonPlayer.BufferedMedia:
            return qsTr("Buffered");
        case DragonPlayer.EndOfMedia:
            return qsTr("End of media");
        case DragonPlayer.InvalidMedia:
            return qsTr("Invalid media");
        default:
            return qsTr("Unknown");
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: urlField
                Layout.fillWidth: true
                placeholderText: qsTr("Audio file or stream URL")
                text: Qt.resolvedUrl("../../tests/fixtures/sample-3s.mp3")
            }

            Button {
                text: qsTr("Load")
                onClicked: {
                    if (urlField.text.length > 0)
                        player.source = urlField.text;
                }
            }
        }

        RowLayout {
            spacing: 8

            Button {
                text: qsTr("Play")
                onClicked: player.play()
            }
            Button {
                text: qsTr("Pause")
                onClicked: player.pause()
            }
            Button {
                text: qsTr("Stop")
                onClicked: player.stop()
            }
        }

        Slider {
            id: positionSlider
            Layout.fillWidth: true
            enabled: player.seekable
            from: 0
            to: Math.max(player.duration.milliseconds, 1)
            value: player.position.milliseconds
            onMoved: player.position.milliseconds = value
        }

        Label {
            text: player.position.formatted() + " / " + player.duration.formatted()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Volume")
            }
            Slider {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: player.audioOutput.volume
                onMoved: player.audioOutput.volume = value
            }
            CheckBox {
                text: qsTr("Muted")
                checked: player.audioOutput.muted
                onToggled: player.audioOutput.muted = checked
            }
        }

        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: root.stateName(player.playbackState) + " | "
                  + root.statusName(player.status) + " | "
                  + player.errorString
        }
    }
}
