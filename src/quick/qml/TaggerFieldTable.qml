// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// The selected files' fields: name, original value and draft. The draft is
// edited in place (double-click, F2, or just typing); edits are staged as
// coloured drafts and written only on Apply. Rows are selected for Remove
// field, Revert and saving a field set.
Rectangle {
    id: fields

    required property QuickTagger tagger
    signal editValues()
    signal addField()

    readonly property int nameWidth: 190
    readonly property int rowHeight: 26

    color: palette.base
    border.color: Shade.mix(palette.window, palette.windowText, 0.15)
    radius: 4

    // A field shown: scrolled to, in the middle.
    function reveal(row) {
        table.positionViewAtRow(row, TableView.AlignVCenter);
        table.forceActiveFocus();
    }

    HorizontalHeaderView {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        syncView: table
        clip: true
        model: [qsTr("Field"), qsTr("Original"), qsTr("Draft")]
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
        objectName: "bench-metadata-fields"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        clip: true
        focus: true
        boundsBehavior: Flickable.StopAtBounds
        model: fields.tagger.fields
        selectionModel: fields.tagger.fieldSelectionModel
        selectionBehavior: TableView.SelectRows
        selectionMode: TableView.ExtendedSelection
        editTriggers: TableView.DoubleTapped | TableView.EditKeyPressed | TableView.AnyKeyPressed
        Accessible.name: qsTr("Metadata fields with original and draft values")
        ScrollBar.vertical: ScrollBar {}

        columnWidthProvider: column => {
            const rest = Math.max(120, (table.width - fields.nameWidth) / 2);
            return column === 0 ? fields.nameWidth : rest;
        }
        rowHeightProvider: row => {
            fields.tagger.filterRevision;
            return fields.tagger.fieldHidden(row) ? 0 : fields.rowHeight;
        }
        onWidthChanged: forceLayout()
        Connections {
            target: fields.tagger
            function onFilterChanged() {
                table.forceLayout();
            }
        }

        Keys.onPressed: event => {
            const control = event.modifiers & Qt.ControlModifier;
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && control) {
                fields.editValues();
                event.accepted = true;
            } else if (event.key === Qt.Key_Insert && event.modifiers === Qt.NoModifier) {
                fields.addField();
                event.accepted = true;
            } else if (event.key === Qt.Key_Delete && event.modifiers === Qt.NoModifier) {
                fields.tagger.removeFields();
                event.accepted = true;
            } else if (event.key === Qt.Key_Backspace && control) {
                fields.tagger.revertFields();
                event.accepted = true;
            }
        }

        delegate: Rectangle {
            id: cell
            required property int row
            required property int column
            required property bool selected
            required property bool current
            required property string display
            required property var toolTip
            required property var foreground
            required property bool bold
            required property bool italic
            required property bool strike
            required property bool placeholder
            required property var edit

            implicitHeight: fields.rowHeight
            color: selected ? Shade.alpha(palette.highlight, 0.35)
                            : (row % 2 ? palette.alternateBase : palette.base)
            clip: true

            Label {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: cell.display
                color: cell.foreground ? cell.foreground
                                       : (cell.placeholder ? palette.placeholderText : palette.text)
                font.bold: cell.bold
                font.italic: cell.italic
                font.strikeout: cell.strike
                opacity: cell.column === 0 && !cell.bold ? 0.85 : 1
            }
            Rectangle {
                visible: cell.current && table.activeFocus
                anchors.fill: parent
                color: "transparent"
                border.color: Shade.alpha(palette.highlight, 0.8)
                border.width: 1
            }
            HoverHandler {
                id: cellHover
            }
            ToolTip.visible: cellHover.hovered && (cell.toolTip ?? "") !== ""
            ToolTip.delay: 900
            ToolTip.text: cell.toolTip ?? ""

            TableView.editDelegate: TextField {
                anchors.fill: parent
                text: cell.edit ?? ""
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                Component.onCompleted: selectAll()
                TableView.onCommit: fields.tagger.setField(cell.row, text)
            }
        }
    }
}
