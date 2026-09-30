// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// Find in the list shown (ADR-0142): any metadata value, the duration, the
// audio format or the path; Enter for the next match, Shift+Enter the one
// before, Escape to close.
Pane {
    id: bar
    objectName: "bench-list-find-bar"

    readonly property var find: Tk.find

    visible: find.shown ?? false
    padding: 4
    leftPadding: 8
    rightPadding: 8

    Connections {
        target: Tk
        function onFindOpened() {
            query.forceActiveFocus();
            query.selectAll();
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 6
        TextField {
            id: query
            objectName: "bench-list-find-query"
            Layout.preferredWidth: 260
            placeholderText: qsTr("Find in current list")
            maximumLength: 1024
            text: bar.find.query ?? ""
            onTextEdited: Tk.setFindQuery(text)
            Accessible.name: qsTr("Find in current list")
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Search any metadata value, the duration, the audio format, or the source path/URI")
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape) {
                    Tk.dismissFind();
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    Tk.findNext((event.modifiers & Qt.ShiftModifier) !== 0);
                    event.accepted = true;
                }
            }
        }
        ToolButton {
            objectName: "action-list-find-previous-button"
            text: qsTr("Previous")
            onClicked: Tk.findNext(true)
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Previous match (Shift+Enter or Shift+F3)")
        }
        ToolButton {
            objectName: "action-list-find-next-button"
            text: qsTr("Next")
            onClicked: Tk.findNext(false)
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Next match (Enter or F3)")
        }
        Label {
            objectName: "bench-list-find-status"
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: bar.find.status ?? ""
        }
        ToolButton {
            objectName: "action-list-find-close"
            text: qsTr("Close")
            onClicked: Tk.dismissFind()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Close find (Escape)")
        }
    }
}
