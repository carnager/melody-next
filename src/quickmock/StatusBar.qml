// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts

Rectangle {
    implicitHeight: 28
    color: Theme.panel

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.line
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 8
        spacing: 2

        Text {
            Layout.fillWidth: true
            text: App.selectionText
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
            elide: Text.ElideRight
        }

        IconButton {
            icon: "queue"
            iconSize: 14
            checked: App.upNextOpen
            tip: "Up Next"
            onClicked: App.upNextOpen = !App.upNextOpen
        }
        IconButton {
            icon: "repeat"
            iconSize: 14
            checked: App.repeat
            tip: "Repeat"
            onClicked: App.repeat = !App.repeat
        }
        IconButton {
            icon: "shuffle"
            iconSize: 14
            checked: App.shuffle
            tip: "Shuffle"
            onClicked: App.shuffle = !App.shuffle
        }
        IconButton {
            icon: "single"
            iconSize: 14
            checked: App.single
            tip: "Stop after current"
            onClicked: App.single = !App.single
        }
        IconButton {
            icon: "consume"
            iconSize: 14
            checked: App.consume
            tip: "Consume"
            onClicked: App.consume = !App.consume
        }

        Rectangle {
            Layout.leftMargin: 6
            implicitWidth: rgLabel.implicitWidth + 20
            implicitHeight: 20
            radius: 10
            color: rgMouse.containsMouse ? Qt.lighter(Theme.raised, 1.15) : Theme.raised
            Text {
                id: rgLabel
                anchors.centerIn: parent
                text: "ReplayGain: " + App.replayGainNames[App.replayGain]
                color: App.replayGain === 0 ? Theme.dim : Theme.text
                font.pixelSize: Theme.smallFontSize
            }
            MouseArea {
                id: rgMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: App.replayGain = (App.replayGain + 1) % 3
            }
        }
    }
}
