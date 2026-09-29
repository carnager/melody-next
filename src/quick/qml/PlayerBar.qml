// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: bar

    readonly property var player: Engine.current.player
    signal menuRequested(Item anchor)

    implicitHeight: 64
    color: Theme.panel

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.line
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12

        Cover {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            source: Engine.current.coverForPath(bar.player.path)
            name: bar.player.album
        }

        Column {
            Layout.preferredWidth: 300
            spacing: 2
            Text {
                width: parent.width
                text: bar.player.entry === "" ? "Nothing playing on " + Engine.current.name : bar.player.title
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: {
                    if (bar.player.error !== "")
                        return bar.player.error;
                    let line = bar.player.artist;
                    if (bar.player.album !== "")
                        line += " — " + bar.player.album;
                    if (bar.player.date !== "")
                        line += " (" + bar.player.date + ")";
                    return line;
                }
                color: bar.player.error !== "" ? Theme.error : Theme.dim
                font.pixelSize: Theme.fontSize
                elide: Text.ElideRight
            }
        }

        IconButton {
            icon: "previous"
            iconSize: 14
            tip: "Previous"
            onClicked: bar.player.previous()
        }

        Rectangle {
            Layout.preferredWidth: 34
            Layout.preferredHeight: 34
            radius: 17
            color: playMouse.pressed ? Qt.darker(Theme.accent, 1.2)
                 : playMouse.containsMouse ? Qt.lighter(Theme.accent, 1.1) : Theme.accent
            scale: playMouse.pressed ? 0.94 : 1
            Behavior on scale { NumberAnimation { duration: 90 } }

            Icon {
                anchors.centerIn: parent
                width: 16
                height: 16
                name: bar.player.playing ? "pause" : "play"
                color: "white"
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.player.toggle()
            }
        }

        IconButton {
            icon: "next"
            iconSize: 14
            tip: "Next"
            onClicked: bar.player.next()
        }

        Text {
            text: Engine.formatDuration(seek.pressed ? seek.value : bar.player.position)
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
            font.features: { "tnum": 1 }
        }

        TkSlider {
            id: seek
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, bar.player.duration)
            enabled: bar.player.entry !== ""
            // Follows the engine except while dragged; the seek is sent once,
            // on release.
            Binding on value {
                when: !seek.pressed
                value: bar.player.position
                restoreMode: Binding.RestoreNone
            }
            onPressedChanged: {
                if (!pressed)
                    bar.player.seek(value);
            }
        }

        Text {
            text: Engine.formatDuration(bar.player.duration)
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
            font.features: { "tnum": 1 }
        }

        Icon {
            Layout.leftMargin: 8
            width: 16
            height: 16
            name: "volume"
            color: Theme.dim
        }

        TkSlider {
            id: volume
            Layout.preferredWidth: 100
            from: 0
            to: 100
            Binding on value {
                when: !volume.pressed
                value: bar.player.volume
                restoreMode: Binding.RestoreNone
            }
            onMoved: bar.player.setVolume(Math.round(value))
        }

        IconButton {
            id: menuButton
            icon: "menu"
            tip: "Menu"
            onClicked: bar.menuRequested(menuButton)
        }
    }
}
