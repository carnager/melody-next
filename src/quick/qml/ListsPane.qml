// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// ADR-0233: the lists as a pane beside the tracks -- every engine's, under
// its name, whether open here or not; one shown by choosing it. Saved ones
// first; an unsaved edit starred; the one playing marked.
Pane {
    id: pane
    objectName: "bench-lists-pane"

    signal newListRequested()
    signal renameRequested(string engine, string id, string name)

    padding: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 4
            Layout.topMargin: 4
            Label {
                Layout.fillWidth: true
                text: qsTr("Lists")
                font.bold: true
            }
            ToolButton {
                objectName: "bench-lists-new"
                text: "+"
                onClicked: pane.newListRequested()
                ToolTip.visible: hovered
                ToolTip.delay: 800
                ToolTip.text: qsTr("New list")
            }
        }
        ListView {
            id: lists
            objectName: "bench-lists-panel"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Tk.listGroups
            ScrollBar.vertical: ScrollBar {}
            delegate: Column {
                id: group
                required property var modelData
                width: lists.width
                Label {
                    leftPadding: 8
                    topPadding: 6
                    bottomPadding: 2
                    text: group.modelData.name
                    font.bold: true
                    opacity: 0.8
                }
                Repeater {
                    model: group.modelData.lists
                    ItemDelegate {
                        id: row
                        required property var modelData
                        width: group.width
                        height: 28
                        leftPadding: 18
                        highlighted: modelData.current
                        onClicked: Tk.openPanelList(group.modelData.engine, modelData.id)
                        // Dropped on: tracks, a library's selection, folders
                        // or files go to this list, opened first when it
                        // is not.
                        DropArea {
                            id: listDrop
                            anchors.fill: parent
                            keys: ["application/x-trackknife-drag", "text/uri-list"]
                            onDropped: drop => {
                                const engine = group.modelData.engine;
                                const id = row.modelData.id;
                                let taken = false;
                                if (Tk.draggedKind() !== "") {
                                    taken = Tk.dropOnPanelList(engine, id);
                                } else {
                                    taken = Tk.dropUrlsOnPanelList(drop.urls, engine, id);
                                }
                                if (taken)
                                    drop.accept(Qt.CopyAction);
                            }
                        }
                        DropMarker {
                            whole: true
                            anchors.fill: parent
                            visible: listDrop.containsDrag
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: {
                                listMenu.engine = group.modelData.engine;
                                listMenu.list = row.modelData;
                                listMenu.popup();
                            }
                        }
                        contentItem: RowLayout {
                            spacing: 6
                            Label {
                                visible: row.modelData.playing
                                text: "▶"
                                color: palette.highlight
                            }
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: row.modelData.name + (row.modelData.dirty ? " *" : "")
                                font.italic: !row.modelData.saved
                                font.bold: row.modelData.open
                            }
                            Label {
                                visible: row.modelData.tracks >= 0
                                text: row.modelData.tracks
                                opacity: 0.6
                            }
                        }
                    }
                }
                Label {
                    visible: group.modelData.note !== ""
                    leftPadding: 18
                    bottomPadding: 4
                    opacity: 0.6
                    text: group.modelData.note
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    listMenu.list = null;
                    listMenu.popup();
                }
            }
        }
    }

    Menu {
        id: listMenu
        property string engine
        property var list: null
        MenuItem {
            visible: listMenu.list !== null && !listMenu.list.open
            height: visible ? implicitHeight : 0
            text: qsTr("Open")
            onTriggered: Tk.openPanelList(listMenu.engine, listMenu.list.id)
        }
        MenuItem {
            visible: listMenu.list !== null && listMenu.list.open
            height: visible ? implicitHeight : 0
            enabled: listMenu.list !== null && !listMenu.list.pinned
            text: qsTr("Close")
            onTriggered: Tk.closePanelList(listMenu.engine, listMenu.list.id)
        }
        MenuItem {
            visible: listMenu.list !== null && listMenu.list.open
                     && (!listMenu.list.saved || listMenu.list.dirty)
            height: visible ? implicitHeight : 0
            text: qsTr("Save")
            onTriggered: Tk.savePanelList(listMenu.engine, listMenu.list.id)
        }
        MenuItem {
            visible: listMenu.list !== null
            height: visible ? implicitHeight : 0
            text: qsTr("Rename…")
            onTriggered: pane.renameRequested(listMenu.engine, listMenu.list.id, listMenu.list.name)
        }
        MenuItem {
            visible: listMenu.list !== null
            height: visible ? implicitHeight : 0
            text: qsTr("Delete…")
            onTriggered: {
                deleteConfirm.engine = listMenu.engine;
                deleteConfirm.id = listMenu.list.id;
                deleteConfirm.text = qsTr("Delete “%1”? Its files are not touched.").arg(listMenu.list.name);
                deleteConfirm.open();
            }
        }
        MenuSeparator {
            visible: listMenu.list !== null
        }
        MenuItem {
            text: qsTr("New list")
            onTriggered: pane.newListRequested()
        }
    }
    Dialog {
        id: deleteConfirm
        objectName: "bench-lists-delete"
        property string engine
        property string id
        property alias text: question.text
        title: qsTr("Delete list")
        anchors.centerIn: Overlay.overlay
        modal: true
        Label {
            id: question
            wrapMode: Text.WordWrap
        }
        footer: DialogButtonBox {
            Button {
                objectName: "bench-lists-delete-confirm"
                text: qsTr("Delete")
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    Tk.deletePanelList(deleteConfirm.engine, deleteConfirm.id);
                    deleteConfirm.close();
                }
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            onRejected: deleteConfirm.close()
        }
    }
}
