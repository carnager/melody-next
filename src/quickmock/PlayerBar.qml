// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: bar

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
            album: App.album
        }

        Column {
            Layout.preferredWidth: 300
            spacing: 2
            Text {
                width: parent.width
                text: App.track.title
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: App.album.artist + " — " + App.album.title + " (" + App.album.year + ")"
                color: Theme.dim
                font.pixelSize: Theme.fontSize
                elide: Text.ElideRight
            }
        }

        IconButton {
            icon: "previous"
            iconSize: 14
            tip: "Previous"
            onClicked: App.previous()
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
                name: App.playing ? "pause" : "play"
                color: "white"
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: App.playing = !App.playing
            }
        }

        IconButton {
            icon: "next"
            iconSize: 14
            tip: "Next"
            onClicked: App.next()
        }

        Text {
            text: MockData.duration(App.elapsed)
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
            font.features: { "tnum": 1 }
        }

        TkSlider {
            Layout.fillWidth: true
            from: 0
            to: App.track.len
            value: App.elapsed
            onMoved: App.elapsed = value
        }

        Text {
            text: MockData.duration(App.track.len)
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
            Layout.preferredWidth: 100
            value: App.volume
            onMoved: App.volume = value
        }

        IconButton {
            id: menuButton
            icon: "menu"
            tip: "Menu"
            onClicked: bar.menuRequested(menuButton)
        }
    }
}
