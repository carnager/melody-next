// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// The device menu: the engine's speakers, once there is a choice of them
// (ADR-0228), then the chosen one's sound devices, then a refresh. Its
// contents are the workspace's (Workspace::outputMenu).
Menu {
    id: outputsMenu

    // Entries are taken out of the menu, not removed: removing destroys an
    // item, and the Instantiator destroys its own again.
    Instantiator {
        model: Tk.outputMenu
        delegate: Loader {
            required property var modelData
            sourceComponent: modelData.kind === "separator" ? separator
                           : modelData.kind === "heading" ? heading : choice
            onLoaded: item.entry = modelData
        }
        onObjectAdded: (index, object) => outputsMenu.insertItem(index, object.item)
        onObjectRemoved: (index, object) => outputsMenu.takeItem(index)
    }
    MenuSeparator {}
    MenuItem {
        objectName: "action-refresh-audio-devices"
        text: "Refresh audio devices"
        onTriggered: Tk.refreshOutputs()
    }

    Component {
        id: separator
        MenuSeparator {
            property var entry
        }
    }
    // Headings as labels, bold at 0.9x.
    Component {
        id: heading
        MenuItem {
            property var entry: ({})
            enabled: false
            text: entry.label ?? ""
            font.bold: true
            font.pointSize: Qt.application.font.pointSize * 0.9
        }
    }
    Component {
        id: choice
        MenuItem {
            property var entry: ({})
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
