// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick

// The tag editor's Artwork page: the pictures in and beside the selected
// files, what is staged for them, and their problems. Covers are staged
// here and saved with Apply (or Save artwork on its own).
ColumnLayout {
    id: page
    objectName: "bench-metadata-artwork-section"

    required property QuickArtwork artwork
    readonly property var state: artwork.state

    signal feedback(string title, string summary, var rows)
    signal folderReview(string note, var rows)

    spacing: 6
    enabled: !(state.working ?? false)

    function openPicker() {
        if (artwork.openPicker())
            picker.open();
    }

    Connections {
        target: page.artwork
        function onFeedbackRequested(title, summary, rows) {
            page.feedback(title, summary, rows);
        }
        function onFolderImagesRequested(note, rows) {
            page.folderReview(note, rows);
        }
    }

    // A table over one of the session's models; pictures come from the
    // artwork image provider, cells of `kind` by row and column.
    component ArtworkTable: Rectangle {
        id: tableFrame
        property alias model: table.model
        property alias selectionModel: table.selectionModel
        property string kind: ""
        property int rowHeight: 26
        property var widths: []
        signal deletePressed()

        color: palette.base
        border.color: Shade.mix(palette.window, palette.windowText, 0.15)
        radius: 4
        clip: true

        HorizontalHeaderView {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            syncView: table
            clip: true
            delegate: Rectangle {
                required property string display
                implicitHeight: 24
                color: Shade.mix(palette.base, palette.window, 0.6)
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: parent.display
                    font.bold: true
                    opacity: 0.8
                    elide: Text.ElideRight
                }
            }
        }
        TableView {
            id: table
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 1
            clip: true
            focus: true
            boundsBehavior: Flickable.StopAtBounds
            selectionBehavior: TableView.SelectRows
            selectionMode: TableView.ExtendedSelection
            ScrollBar.vertical: ScrollBar {}
            rowHeightProvider: () => tableFrame.rowHeight
            columnWidthProvider: column => {
                const fixed = tableFrame.widths[column];
                if (fixed > 0)
                    return fixed;
                let used = 0;
                let flexible = 0;
                for (let at = 0; at < table.columns; ++at) {
                    if ((tableFrame.widths[at] ?? 0) > 0)
                        used += tableFrame.widths[at];
                    else
                        ++flexible;
                }
                return Math.max(80, (table.width - used) / Math.max(1, flexible));
            }
            onWidthChanged: forceLayout()
            Keys.onDeletePressed: tableFrame.deletePressed()
            delegate: Rectangle {
                id: cell
                required property int row
                required property int column
                required property bool selected
                required property var display
                required property var toolTip
                required property var decoration
                required property var font
                color: selected ? Shade.alpha(palette.highlight, 0.35)
                                : (row % 2 ? palette.alternateBase : palette.base)
                clip: true
                readonly property bool pictured: decoration !== undefined && decoration !== null
                Image {
                    anchors.centerIn: parent
                    visible: cell.pictured
                    width: Math.min(parent.width - 8, 60)
                    height: Math.min(parent.height - 8, 60)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    source: cell.pictured
                            ? "image://artwork/%1/%2/%3/%4?%5".arg(page.artwork.id).arg(tableFrame.kind)
                                  .arg(cell.row).arg(cell.column).arg(page.artwork.revision)
                            : ""
                }
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    visible: !cell.pictured
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideMiddle
                    text: cell.display ?? ""
                    font.strikeout: cell.font ? cell.font.strikeout : false
                }
                HoverHandler {
                    id: hover
                }
                ToolTip.visible: hover.hovered && (cell.toolTip ?? "") !== ""
                ToolTip.delay: 800
                ToolTip.text: cell.toolTip ?? ""
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            objectName: "bench-metadata-artwork-status"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: page.state.status ?? ""
        }
        ProgressBar {
            objectName: "bench-metadata-artwork-progress"
            visible: page.state.progressVisible ?? false
            Layout.preferredWidth: 170
            indeterminate: (page.state.progressMaximum ?? 0) === 0
            from: 0
            to: Math.max(1, page.state.progressMaximum ?? 1)
            value: page.state.progress ?? 0
        }
        Button {
            objectName: "bench-metadata-artwork-stop"
            text: qsTr("Stop")
            visible: page.state.progressVisible ?? false
            enabled: page.state.canStop ?? false
            onClicked: page.artwork.stop()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Stop after the files already in progress are safe")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Item {
            Layout.fillWidth: true
        }
        Button {
            objectName: "bench-metadata-artwork-fetch-cover"
            text: qsTr("Fetch cover…")
            enabled: page.state.canFetch ?? false
            onClicked: page.openPicker()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Choose a cover: an image beside the files, or one the Cover Art Archive has for the release. A front cover replaces the existing one.")
        }
        Button {
            objectName: "bench-metadata-artwork-add"
            text: qsTr("Add image…")
            enabled: page.state.canAdd ?? false
            onClicked: addFile.open()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Add one PNG or JPEG to every selected writable file")
        }
        Button {
            objectName: "bench-metadata-artwork-copy"
            text: qsTr("Copy to Selection")
            enabled: page.state.canCopy ?? false
            onClicked: page.artwork.copySelected()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Add the selected image to the other selected writable files")
        }
        Button {
            objectName: "bench-metadata-artwork-export"
            text: qsTr("Export…")
            enabled: page.state.canExport ?? false
            onClicked: exportFolder.open()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Export selected encoded images without overwriting existing files")
        }
        Button {
            objectName: "bench-metadata-artwork-replace"
            text: qsTr("Replace…")
            enabled: page.state.canReplace ?? false
            onClicked: replaceFile.open()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Replace selected embedded covers; external image files are kept with one PNG or JPEG")
        }
        Button {
            objectName: "bench-metadata-artwork-remove"
            text: qsTr("Remove")
            enabled: page.state.canRemove ?? false
            onClicked: page.artwork.removeSelected()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Remove selected embedded covers; external image files are kept")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.75
            text: page.state.draftHelp ?? ""
        }
        Button {
            objectName: "bench-metadata-artwork-undo-selected"
            text: qsTr("Undo selected")
            enabled: page.state.canUndoSelected ?? false
            onClicked: page.artwork.undoSelected()
            ToolTip.visible: hovered
            ToolTip.delay: 800
            ToolTip.text: qsTr("Undo the selected pending changes")
        }
        Button {
            objectName: "bench-metadata-artwork-discard"
            text: qsTr("Discard changes")
            enabled: page.state.canDiscard ?? false
            onClicked: page.artwork.discard()
        }
        Button {
            objectName: "bench-metadata-artwork-save"
            text: qsTr("Save artwork")
            visible: page.state.saveVisible ?? true
            enabled: page.state.canSave ?? false
            onClicked: page.artwork.save()
        }
    }

    ArtworkTable {
        objectName: "bench-metadata-artwork-pending"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(240, 26 + Math.max(1, page.artwork.pending.rowCount()) * 68)
        visible: page.state.pendingVisible ?? false
        model: page.artwork.pending
        selectionModel: page.artwork.pendingSelection
        kind: "pending"
        rowHeight: 68
        widths: [0, 0, 0, 0, 92, 92]
        onDeletePressed: if (page.state.canUndoSelected) page.artwork.undoSelected()
    }

    ArtworkTable {
        objectName: "bench-metadata-artwork-items"
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !(page.state.emptyVisible ?? true)
        model: page.artwork.items
        selectionModel: page.artwork.itemSelection
        kind: "items"
        rowHeight: 68
        widths: [72, 0, 90, 190, 0]
        onDeletePressed: if (page.state.canRemove) page.artwork.removeSelected()
    }

    Label {
        objectName: "bench-metadata-artwork-empty"
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: page.state.emptyVisible ?? true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        opacity: 0.7
        text: page.state.emptyText ?? ""
    }

    ColumnLayout {
        objectName: "bench-metadata-artwork-issues-pane"
        Layout.fillWidth: true
        visible: page.state.issuesVisible ?? false
        spacing: 4
        Label {
            text: qsTr("Problems")
            font.bold: true
        }
        ArtworkTable {
            objectName: "bench-metadata-artwork-issues"
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            model: page.artwork.issues
            kind: "issues"
            widths: [0, 0, 110, 0]
        }
    }

    FileDialog {
        id: addFile
        title: qsTr("Choose artwork to add")
        nameFilters: [qsTr("Artwork images (*.png *.jpg *.jpeg)"), qsTr("All files (*)")]
        onAccepted: {
            roleDialog.file = selectedFile;
            roleDialog.open();
        }
    }
    Dialog {
        id: roleDialog
        property url file
        title: qsTr("Artwork role")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: qsTr("Store as:")
            }
            ComboBox {
                id: roleChoice
                Layout.preferredWidth: 240
                model: page.artwork.addRoles
            }
        }
        onOpened: roleChoice.currentIndex = 0
        onAccepted: page.artwork.add(file, roleChoice.currentIndex)
    }
    FileDialog {
        id: replaceFile
        title: qsTr("Choose replacement artwork")
        nameFilters: [qsTr("Artwork images (*.png *.jpg *.jpeg)"), qsTr("All files (*)")]
        onAccepted: page.artwork.replace(selectedFile)
    }
    FolderDialog {
        id: exportFolder
        title: qsTr("Choose artwork export directory")
        onAccepted: page.artwork.exportSelected(selectedFolder)
    }
    CoverPicker {
        id: picker
        artwork: page.artwork
    }
}
