// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick

// The files being edited: their common folder once above them, each named
// relative to it. Checked files receive new edits: a click on a name selects
// it (Shift or Ctrl for more), a click on its box -- or Space -- toggles it.
// Apply still saves every staged edit.
Rectangle {
    id: files

    required property QuickTagger tagger

    color: palette.base
    border.color: Shade.mix(palette.window, palette.windowText, 0.15)
    radius: 4
    implicitWidth: 260

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 1
        spacing: 0

        Label {
            objectName: "bench-metadata-files-dir"
            Layout.fillWidth: true
            Layout.margins: 8
            Layout.bottomMargin: 4
            visible: text !== ""
            text: files.tagger.commonFolder
            elide: Text.ElideMiddle
            opacity: 0.7
            font.pointSize: Qt.application.font.pointSize * 0.9
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

            delegate: Rectangle {
                id: row
                required property int index
                readonly property bool chosen: {
                    files.tagger.fileSelection;
                    return files.tagger.fileSelected(index);
                }
                width: ListView.view.width
                height: 32
                color: rowHover.hovered ? Shade.alpha(palette.highlight, 0.12)
                                        : (index % 2 ? Shade.mix(palette.base, palette.alternateBase, 1)
                                                     : palette.base)
                border.width: ListView.isCurrentItem && list.activeFocus ? 1 : 0
                border.color: Shade.alpha(palette.highlight, 0.6)

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
                    spacing: 6
                    CheckBox {
                        Layout.leftMargin: 6
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
                        opacity: row.chosen ? 1 : 0.65
                    }
                }
                ToolTip.visible: rowHover.hovered
                ToolTip.delay: 900
                ToolTip.text: qsTr("Checked files receive new edits. Click a checkbox or press Space to toggle a file. Click a filename to select it; use Shift/Ctrl for multiple files. Apply still saves all staged edits.")
            }
        }
    }
}
