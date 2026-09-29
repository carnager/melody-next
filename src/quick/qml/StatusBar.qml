// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: bar

    property string text
    property bool upNextOpen
    signal upNextToggled

    readonly property var player: Engine.current.player
    readonly property var modeNames: ["off", "on", "once"]

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
            text: bar.text
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
            elide: Text.ElideRight
        }

        IconButton {
            icon: "queue"
            iconSize: 14
            checked: bar.upNextOpen
            tip: "Up Next"
            onClicked: bar.upNextToggled()
        }
        IconButton {
            icon: "repeat"
            iconSize: 14
            checked: bar.player.repeat
            tip: "Repeat"
            onClicked: bar.player.setRepeat(!bar.player.repeat)
        }
        IconButton {
            icon: "shuffle"
            iconSize: 14
            checked: bar.player.random
            tip: "Shuffle"
            onClicked: bar.player.setRandom(!bar.player.random)
        }
        IconButton {
            icon: "single"
            iconSize: 14
            checked: bar.player.single !== 0
            opacity: bar.player.single === 2 ? 0.7 : 1
            tip: "Stop after current: " + bar.modeNames[bar.player.single]
            onClicked: bar.player.setSingle((bar.player.single + 1) % 3)
        }
        IconButton {
            icon: "consume"
            iconSize: 14
            checked: bar.player.consume !== 0
            opacity: bar.player.consume === 2 ? 0.7 : 1
            tip: "Consume: " + bar.modeNames[bar.player.consume]
            onClicked: bar.player.setConsume((bar.player.consume + 1) % 3)
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
                text: "ReplayGain: " + bar.player.replayGain.charAt(0).toUpperCase() + bar.player.replayGain.slice(1)
                color: bar.player.replayGain === "off" ? Theme.dim : Theme.text
                font.pixelSize: Theme.smallFontSize
            }
            MouseArea {
                id: rgMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.player.cycleReplayGain()
            }
        }
    }
}
