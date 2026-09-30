// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A window listing rows of text under headers, with a note above: loudness
// sources, folder covers to review, files that need attention. Reviewing
// asks Save or Cancel; otherwise it only closes.
Dialog {
    id: dialog

    property string note: ""
    property var headers: []
    property var rows: []
    property bool review: false
    // A button of its own, left of Close; empty for none.
    property string extraButton: ""
    signal extra()

    function standardButton(which) {
        return buttons.standardButton(which);
    }
    function show(title, note, headers, rows, review) {
        dialog.title = title;
        dialog.note = note;
        dialog.headers = headers;
        dialog.rows = rows;
        dialog.review = review;
        open();
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(760, Overlay.overlay ? Overlay.overlay.width - 40 : 760)
    height: rows.length === 0 ? 170 : Math.min(420, Overlay.overlay ? Overlay.overlay.height - 40 : 420)
    modal: true
    standardButtons: review ? Dialog.Save | Dialog.Cancel : Dialog.Close

    footer: DialogButtonBox {
        id: buttons
        standardButtons: dialog.standardButtons
        Button {
            visible: dialog.extraButton !== ""
            text: dialog.extraButton
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: dialog.extra()
        }
        onAccepted: dialog.accept()
        onRejected: dialog.reject()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label {
            objectName: "bench-preparation-feedback-summary"
            Layout.fillWidth: true
            visible: dialog.note !== ""
            wrapMode: Text.WordWrap
            text: dialog.note
        }
        HorizontalHeaderView {
            id: header
            Layout.fillWidth: true
            visible: dialog.rows.length > 0
            syncView: table
            clip: true
            model: dialog.headers
            delegate: Rectangle {
                required property string modelData
                implicitHeight: 26
                color: Shade.mix(palette.base, palette.window, 0.6)
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: parent.modelData
                    font.bold: true
                    opacity: 0.8
                }
            }
        }
        TableView {
            id: table
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.rows.length > 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            model: TextRows {
                rows: dialog.rows
            }
            columnWidthProvider: column => Math.max(90, table.width / Math.max(1, dialog.headers.length))
            onWidthChanged: forceLayout()
            delegate: Rectangle {
                required property string display
                required property int row
                implicitHeight: 26
                color: row % 2 ? palette.alternateBase : palette.base
                clip: true
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                    text: parent.display
                }
                HoverHandler {
                    id: hover
                }
                ToolTip.visible: hover.hovered
                ToolTip.delay: 700
                ToolTip.text: display
            }
        }
    }
}
