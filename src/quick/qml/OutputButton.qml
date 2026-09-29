// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Where the current engine plays (ADR-0228): which of its outputs -- its
// own audio or an agent -- and which sound device there.
Rectangle {
    id: button

    readonly property var session: Engine.current
    readonly property var player: session.player
    readonly property var chosen: {
        for (const output of player.outputs)
            if (output.selected)
                return output;
        return null;
    }
    readonly property bool agents: player.outputs.some(output => !output.local)
    // A sink as people know it: its description, where it has one.
    function describe(name) {
        for (const device of player.devices)
            if (device.name === name)
                return device.description !== "" ? device.description : device.name;
        return name;
    }
    readonly property string deviceName: player.device !== "" ? describe(player.device)
                                       : player.defaultDevice !== "" ? describe(player.defaultDevice)
                                       : "System default"

    implicitWidth: Math.min(260, row.implicitWidth + 16)
    implicitHeight: 22
    radius: 4
    color: mouse.containsMouse || popup.opened ? Theme.hover : "transparent"

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 6
        Icon {
            width: 13
            height: 13
            name: "speaker"
            color: button.player.outputAvailable ? Theme.dim : Theme.error
        }
        Text {
            Layout.fillWidth: true
            text: (button.agents && button.chosen ? button.chosen.name + " · " : "") + button.deviceName
                  + (button.chosen && !button.chosen.online ? " (offline)" : "")
            elide: Text.ElideMiddle
            color: Theme.dim
            font.pixelSize: Theme.smallFontSize
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.opened ? popup.close() : popup.open()
    }

    ToolTip.visible: mouse.containsMouse && !popup.opened
    ToolTip.delay: 600
    ToolTip.text: button.session.name + " → " + (button.chosen ? button.chosen.name + " → " : "") + button.deviceName
                  + (!button.player.outputAvailable ? "\nPaused until an output is available" : "")
                  + (button.player.speakersTakenBy !== "" ? "\nThe speakers were taken by " + button.player.speakersTakenBy : "")

    Popup {
        id: popup
        y: -height - 4
        x: button.width - width
        width: 300
        padding: 6
        background: Rectangle {
            color: Theme.panel
            radius: 8
            border.color: Theme.line
        }

        contentItem: Column {
            spacing: 1

            component Heading: Text {
                width: parent.width
                leftPadding: 8
                topPadding: 6
                bottomPadding: 4
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.5
            }
            component Choice: Rectangle {
                id: choice
                property string label
                property string hint
                property bool checked
                signal chosen
                width: parent.width
                height: 28
                radius: 4
                color: choiceMouse.containsMouse ? Theme.hover : "transparent"
                Icon {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 13
                    height: 13
                    name: "check"
                    color: Theme.accent
                    visible: choice.checked
                }
                Column {
                    x: 28
                    width: parent.width - x - 8
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: choice.label
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                    }
                }
                MouseArea {
                    id: choiceMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        choice.chosen();
                        popup.close();
                    }
                }
                ToolTip.visible: choiceMouse.containsMouse && choice.hint !== ""
                ToolTip.delay: 600
                ToolTip.text: choice.hint
            }

            Heading {
                visible: button.agents
                text: "Speakers"
            }
            Repeater {
                model: button.agents ? button.player.outputs : []
                delegate: Choice {
                    required property var modelData
                    label: modelData.name + (modelData.online ? "" : " (offline)")
                    // An offline agent can still be chosen: the music waits
                    // there and starts when it is back.
                    hint: !modelData.online ? "Not connected. Chosen, it plays as soon as it is back."
                        : !modelData.local && !modelData.files ? "Streams the music from the engine" : ""
                    checked: modelData.selected
                    onChosen: button.player.selectOutput(modelData.id)
                }
            }
            Heading {
                text: button.agents && button.chosen ? "Sound device on " + button.chosen.name : "Sound device"
            }
            Choice {
                label: "System default" + (button.player.defaultDevice !== ""
                                           ? " (" + button.describe(button.player.defaultDevice) + ")" : "")
                checked: button.player.device === ""
                onChosen: button.player.setDevice("")
            }
            Repeater {
                model: button.player.devices
                delegate: Choice {
                    required property var modelData
                    label: modelData.description !== "" ? modelData.description : modelData.name
                    checked: button.player.device === modelData.name
                    onChosen: button.player.setDevice(modelData.name)
                }
            }
        }
    }
}
