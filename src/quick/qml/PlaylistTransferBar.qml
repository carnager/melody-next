// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// An M3U8 import or export under the window while it runs, and what came
// of it until closed.
Pane {
    id: bar
    objectName: "bench-playlist-transfer"

    readonly property var transfer: Tk.transfer

    visible: transfer.shown ?? false
    padding: 6
    leftPadding: 12
    RowLayout {
        anchors.fill: parent
        Label {
            objectName: "bench-playlist-transfer-status"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: bar.transfer.status ?? ""
        }
        Button {
            objectName: "action-cancel-playlist-transfer"
            text: (bar.transfer.active ?? false) ? qsTr("Cancel") : qsTr("Close")
            onClicked: Tk.dismissTransfer()
        }
    }
}
