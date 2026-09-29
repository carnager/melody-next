// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic

// Asks before something that cannot be taken back.
Dialog {
    id: dialog

    property string message
    property var action: null

    function ask(title, message, action) {
        dialog.title = title;
        dialog.message = message;
        dialog.action = action;
        open();
    }

    modal: true
    anchors.centerIn: Overlay.overlay
    width: 420
    standardButtons: Dialog.Yes | Dialog.Cancel
    onAccepted: {
        if (action)
            action();
    }

    background: Rectangle {
        color: Theme.panel
        radius: 8
        border.color: Theme.line
    }

    contentItem: Text {
        text: dialog.message
        wrapMode: Text.WordWrap
        color: Theme.text
        font.pixelSize: Theme.fontSize
    }
}
