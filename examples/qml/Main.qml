/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.dragonqmlexample.types

ApplicationWindow {
    id: root

    width: 900
    height: 640
    visible: true
    title: qsTr("dragon-media-backend QML example")

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

    function backendName(backend) {
        switch (backend) {
        case 1:
            return qsTr("PipeWire");
        case 2:
            return qsTr("PulseAudio");
        case 3:
            return qsTr("SDL");
        default:
            return qsTr("Auto");
        }
    }

    function openAndPlay(source) {
        if (source.toString().length === 0)
            return;
        player.source = source;
        player.play();
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Open audio file")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Audio files (*.mp3 *.ogg *.flac *.opus *.aac *.m4a *.wma *.wav)"), qsTr("All files (*)")]
        onAccepted: {
            urlField.text = selectedFile.toString();
            root.openAndPlay(selectedFile);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            TextField {
                id: urlField
                Layout.fillWidth: true
                placeholderText: qsTr("Audio file URL or http(s) stream URL")
                text: defaultUrl.toString()
            }

            Button {
                text: qsTr("Open")
                onClicked: root.openAndPlay(urlField.text)
            }

            Button {
                text: qsTr("Browse…")
                onClicked: fileDialog.open()
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
                value: audioOutput.volume
                onMoved: audioOutput.volume = value
            }
            CheckBox {
                text: qsTr("Muted")
                checked: audioOutput.muted
                onToggled: audioOutput.muted = checked
            }
        }

        SpectrogramView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visualization: spectrum
        }

        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: root.stateName(player.playbackState) + " | "
                  + root.statusName(player.status) + " | "
                  + player.errorString + " | "
                  + qsTr("sink: ") + root.backendName(audioOutput.backend)
        }
    }
}
