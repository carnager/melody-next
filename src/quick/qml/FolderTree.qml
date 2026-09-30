// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// bench-folder-tree: the filesystem, a level at a time. A file opened goes
// into this computer's list; a folder can be added, opened or bookmarked.
Rectangle {
    id: pane
    color: palette.base

    TreeView {
        id: tree
        objectName: "bench-folder-tree"
        anchors.fill: parent
        clip: true
        model: Tk.folders.model
        boundsBehavior: Flickable.StopAtBounds
        selectionModel: ItemSelectionModel {}
        ScrollBar.vertical: ScrollBar {}

        function openRow(row, open) {
            const index = tree.index(row, 0);
            if (open) {
                if (Tk.folders.model.canFetchMore(index))
                    Tk.folders.model.fetchMore(index);
                tree.expand(row);
            } else {
                tree.collapse(row);
            }
        }

        delegate: Item {
            id: node
            required property TreeView treeView
            required property bool isTreeNode
            required property bool expanded
            required property bool hasChildren
            required property int depth
            required property int row
            required property bool current
            required property bool selected
            required property string display
            implicitWidth: tree.width
            implicitHeight: 24

            Rectangle {
                anchors.fill: parent
                visible: node.selected
                color: node.palette.highlight
            }
            Label {
                id: arrow
                x: 4 + node.depth * 16
                width: 14
                anchors.verticalCenter: parent.verticalCenter
                visible: node.hasChildren
                text: node.expanded ? "▾" : "▸"
                color: node.selected ? node.palette.highlightedText : node.palette.placeholderText
                TapHandler {
                    onTapped: tree.openRow(node.row, !node.expanded)
                }
            }
            Image {
                id: icon
                x: arrow.x + 16
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                sourceSize: Qt.size(16, 16)
                source: "image://icon/" + (node.hasChildren ? "folder" : "audio-x-generic|text-x-generic")
            }
            Label {
                x: icon.x + 22
                width: parent.width - x - 4
                anchors.verticalCenter: parent.verticalCenter
                text: node.display
                elide: Text.ElideRight
                color: node.selected ? node.palette.highlightedText : node.palette.text
            }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                // Activated as the desktop activates items: on a click where
                // it does so on one, else on a double click -- a folder
                // opens, a file goes into this computer's list.
                function activate() {
                    const index = tree.index(node.row, 0);
                    if (Tk.folders.isDirectory(index))
                        tree.openRow(node.row, !node.expanded);
                    else
                        Tk.openFolderEntry(index);
                }
                onTapped: {
                    tree.selectionModel.setCurrentIndex(tree.index(node.row, 0),
                                                        ItemSelectionModel.ClearAndSelect
                                                        | ItemSelectionModel.Rows);
                    if (Qt.styleHints.singleClickActivation)
                        activate();
                }
                onDoubleTapped: {
                    if (!Qt.styleHints.singleClickActivation)
                        activate();
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    const index = tree.index(node.row, 0);
                    tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                        | ItemSelectionModel.Rows);
                    folderMenu.index = index;
                    folderMenu.row = node.row;
                    folderMenu.directory = Tk.folders.isDirectory(index);
                    folderMenu.expanded = node.expanded;
                    folderMenu.popup();
                }
            }
        }

        Connections {
            target: Tk.folders
            function onExpandRequested(index) {
                tree.expandToIndex(index);
                const row = tree.rowAtIndex(index);
                if (row >= 0)
                    tree.expand(row);
            }
            function onCurrentRequested(index) {
                tree.expandToIndex(index);
                tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                    | ItemSelectionModel.Rows);
                const row = tree.rowAtIndex(index);
                if (row >= 0)
                    tree.positionViewAtRow(row, TableView.Contain);
            }
        }
    }

    Menu {
        id: folderMenu
        property var index
        property int row: -1
        property bool directory: false
        property bool expanded: false
        MenuItem {
            objectName: "action-folder-add-to-list"
            text: folderMenu.directory ? "Add folder to current list" : "Add file to current list"
            onTriggered: Tk.openFolderEntry(folderMenu.index)
        }
        MenuItem {
            objectName: "action-folder-toggle-expanded"
            visible: folderMenu.directory
            height: visible ? implicitHeight : 0
            text: folderMenu.expanded ? "Collapse" : "Expand"
            onTriggered: tree.openRow(folderMenu.row, !folderMenu.expanded)
        }
        MenuItem {
            objectName: "action-folder-bookmark-add"
            visible: folderMenu.directory
            height: visible ? implicitHeight : 0
            text: "Bookmark folder"
            onTriggered: Tk.folders.addBookmark(folderMenu.index)
        }
    }
}
