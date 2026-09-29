// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Asks for a name: a new list's, or a new one for a list.
Dialog {
    id: dialog

    property string label
    property var action: null

    function ask(title, label, initial, action) {
        dialog.title = title;
        dialog.label = label;
        dialog.action = action;
        field.text = initial;
        open();
        field.selectAll();
        field.forceActiveFocus();
    }

    modal: true
    anchors.centerIn: Overlay.overlay
    width: 400
    standardButtons: Dialog.Ok | Dialog.Cancel
    onAccepted: {
        if (field.text.trim() !== "" && action)
            action(field.text.trim());
    }

    background: Rectangle {
        color: Theme.panel
        radius: 8
        border.color: Theme.line
    }

    contentItem: ColumnLayout {
        spacing: 8
        Text {
            text: dialog.label
            color: Theme.dim
            font.pixelSize: Theme.fontSize
        }
        TextField {
            id: field
            Layout.fillWidth: true
            color: Theme.text
            font.pixelSize: Theme.fontSize
            onAccepted: dialog.accept()
            background: Rectangle {
                radius: 5
                color: Theme.window
                border.color: field.activeFocus ? Theme.accent : Theme.line
            }
        }
    }
}
