// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// The files being edited: their common folder once above them, each named
// relative to it. Checked files receive new edits: a click on a name selects
// it (Shift or Ctrl for more), a click on its box -- or Space -- toggles it.
// Apply still saves every staged edit.
Rectangle {
    id: files

    required property QuickTagger tagger

    // A side pane, a shade off the window, as the settings' pages are.
    color: Theme.sunken(palette)
    implicitWidth: 260

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.gap
        anchors.rightMargin: Theme.gap
        spacing: 0

        Label {
            objectName: "bench-metadata-files-dir"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gap
            Layout.rightMargin: Theme.gap
            Layout.topMargin: Theme.gap + 4
            Layout.bottomMargin: Theme.gap
            visible: text !== ""
            text: files.tagger.commonFolder
            elide: Text.ElideMiddle
            color: Theme.dim(palette)
            font.pointSize: Theme.smallSize
            ToolTip.visible: dirHover.hovered
            ToolTip.text: text
            HoverHandler {
                id: dirHover
            }
        }

        ListView {
            id: list
            objectName: "bench-metadata-files"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: files.tagger.files
            currentIndex: 0
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            Accessible.name: qsTr("Files included in metadata edit")

            Keys.onSpacePressed: files.tagger.clickFile(currentIndex, 0, true)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                    files.tagger.selectAllFiles();
                    event.accepted = true;
                }
            }

            spacing: 1
            delegate: Rectangle {
                id: row
                required property int index
                readonly property bool chosen: {
                    files.tagger.fileSelection;
                    return files.tagger.fileSelected(index);
                }
                width: ListView.view.width
                height: Theme.rowHeight + 2
                radius: Theme.radius
                color: rowHover.hovered ? Theme.hovered(palette) : "transparent"
                border.width: ListView.isCurrentItem && list.activeFocus ? 1 : 0
                border.color: Theme.alpha(palette.highlight, 0.6)

                HoverHandler {
                    id: rowHover
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: mouse => {
                        list.currentIndex = row.index;
                        list.forceActiveFocus();
                        files.tagger.clickFile(row.index, mouse.modifiers, false);
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    spacing: Theme.gapSmall
                    CheckBox {
                        Layout.leftMargin: Theme.gapSmall
                        checked: row.chosen
                        focusPolicy: Qt.NoFocus
                        onClicked: {
                            list.currentIndex = row.index;
                            files.tagger.clickFile(row.index, 0, true);
                            checked = Qt.binding(() => row.chosen);
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.rightMargin: 8
                        text: {
                            files.tagger.commonFolder;
                            return files.tagger.fileText(row.index);
                        }
                        elide: Text.ElideRight
                        color: row.chosen ? palette.windowText : Theme.dim(palette)
                    }
                }
                ToolTip.visible: rowHover.hovered
                ToolTip.delay: 900
                ToolTip.text: qsTr("Checked files receive new edits. Click a checkbox or press Space to toggle a file. Click a filename to select it; use Shift/Ctrl for multiple files. Apply still saves all staged edits.")
            }
        }
    }
}
