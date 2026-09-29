// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// The device menu: the engine's speakers, once there is a choice of them
// (ADR-0228), then the chosen one's sound devices, then a refresh. Its
// contents are the workspace's (Workspace::outputMenu).
Menu {
    id: menu

    Instantiator {
        model: Tk.outputMenu
        delegate: Component {
            Loader {
                required property var modelData
                sourceComponent: modelData.kind === "separator" ? separator
                               : modelData.kind === "heading" ? heading : choice
                property var entry: modelData
            }
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object.item)
        onObjectRemoved: (index, object) => menu.removeItem(object.item)
    }
    MenuSeparator {}
    MenuItem {
        objectName: "action-refresh-audio-devices"
        text: "Refresh audio devices"
        onTriggered: Tk.refreshOutputs()
    }

    Component {
        id: separator
        MenuSeparator {}
    }
    // Headings as labels, bold at 0.9x in the quiet colour.
    Component {
        id: heading
        MenuItem {
            enabled: false
            text: parent ? parent.entry.label : ""
            font.bold: true
            font.pointSize: Qt.application.font.pointSize * 0.9
        }
    }
    Component {
        id: choice
        MenuItem {
            readonly property var entry: parent ? parent.entry : ({})
            text: entry.label ?? ""
            checkable: true
            checked: entry.checked ?? false
            enabled: entry.enabled ?? true
            ToolTip.visible: hovered && (entry.tooltip ?? "") !== ""
            ToolTip.text: entry.tooltip ?? ""
            onTriggered: {
                if (entry.kind === "speaker")
                    Tk.selectOutput(entry.id);
                else
                    Tk.setOutputDevice(entry.target);
            }
        }
    }
}
