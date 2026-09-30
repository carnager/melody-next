// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// Commands: every workspace command by name or shortcut, run from the
// keyboard.
Dialog {
    id: commandPalette
    objectName: "command-palette"

    // {id, label, shortcut, enabled, checked, run}, as the window has them.
    property var commands: []
    readonly property var shown: {
        const filter = filterField.text;
        return commands.filter(command => command.enabled
                               && Tk.commandMatches(filter, command.label, command.shortcut, command.id));
    }
    readonly property var current: list.currentIndex >= 0 && list.currentIndex < shown.length
                                   ? shown[list.currentIndex] : null

    title: qsTr("Commands")
    anchors.centerIn: Overlay.overlay
    width: 620
    height: 480
    modal: true
    focus: true

    onOpened: {
        filterField.text = "";
        filterField.forceActiveFocus();
        list.currentIndex = 0;
    }
    function runCurrent() {
        const command = current;
        if (!command)
            return;
        close();
        command.run();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        TextField {
            id: filterField
            objectName: "command-filter"
            Layout.fillWidth: true
            placeholderText: qsTr("Find a command by name or shortcut…")
            Accessible.name: qsTr("Command search")
            onTextChanged: list.currentIndex = 0
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                    if (list.count > 0)
                        list.currentIndex = Math.max(0, Math.min(list.count - 1,
                                                    list.currentIndex + (event.key === Qt.Key_Down ? 1 : -1)));
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    commandPalette.runCurrent();
                    event.accepted = true;
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base
            border.color: Shade.mix(palette.window, palette.windowText, 0.15)
            radius: 4
            ListView {
                id: list
                objectName: "command-results"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: commandPalette.shown
                highlightMoveDuration: 0
                Accessible.name: qsTr("Matching commands")
                delegate: ItemDelegate {
                    id: row
                    required property var modelData
                    required property int index
                    width: list.width
                    highlighted: ListView.isCurrentItem
                    onClicked: list.currentIndex = index
                    onDoubleClicked: {
                        list.currentIndex = index;
                        commandPalette.runCurrent();
                    }
                    contentItem: RowLayout {
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.modelData.label + (row.modelData.checked ? qsTr(" · On") : "")
                        }
                        Label {
                            text: row.modelData.shortcut
                            opacity: 0.7
                        }
                    }
                }
            }
        }
        Label {
            objectName: "command-status"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: commandPalette.current ? qsTr("Press Enter to run this command.") : qsTr("No matching command")
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Change shortcuts in Settings → Shortcuts.")
        }
    }
    footer: DialogButtonBox {
        Button {
            objectName: "command-run"
            text: qsTr("Run")
            enabled: commandPalette.current !== null
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Close")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        onAccepted: commandPalette.runCurrent()
        onRejected: commandPalette.close()
    }
}
