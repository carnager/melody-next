// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// "Edit local list", under the lists: a tkfmt-1 sort expression and its
// direction, what the edit is doing, and a way to stop it or close the bar.
ToolBar {
    id: bar
    objectName: "bench-list-edit-bar"

    property bool sortShown: false

    function openSort() {
        sortShown = true;
        visible = true;
        expression.forceActiveFocus();
        expression.selectAll();
    }
    function sort() {
        Tk.edit.sort(expression.text, direction.currentIndex === 1);
    }

    visible: false
    position: ToolBar.Footer

    Connections {
        target: Tk.edit
        function onShown() {
            bar.sortShown = Tk.edit.sorting;
            if (Tk.edit.sorting) {
                expression.text = Tk.edit.expression;
                direction.currentIndex = Tk.edit.descending ? 1 : 0;
            }
            bar.visible = true;
        }
    }

    RowLayout {
        anchors.fill: parent
        TextField {
            id: expression
            objectName: "bench-list-sort-expression"
            visible: bar.sortShown
            Layout.minimumWidth: 240
            text: "%title%"
            maximumLength: 4096
            enabled: !Tk.edit.active
            Accessible.name: qsTr("tkfmt-1 sorting expression")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Sort the entire list using tkfmt-1. Example: %album%|%tracknumber%|%title%")
            onAccepted: bar.sort()
        }
        ComboBox {
            id: direction
            objectName: "bench-list-sort-direction"
            visible: bar.sortShown
            model: [qsTr("Ascending"), qsTr("Descending")]
            enabled: !Tk.edit.active
            Accessible.name: qsTr("Sort direction")
        }
        ToolButton {
            objectName: "action-apply-list-sort"
            visible: bar.sortShown
            text: qsTr("Sort list")
            enabled: !Tk.edit.active
            onClicked: bar.sort()
        }
        Label {
            objectName: "bench-list-edit-status"
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            text: Tk.edit.status
            elide: Text.ElideRight
        }
        ToolButton {
            objectName: "action-cancel-list-edit"
            text: Tk.edit.active ? qsTr("Cancel") : qsTr("Close")
            onClicked: {
                Tk.edit.cancel();
                bar.visible = false;
            }
        }
    }
}
