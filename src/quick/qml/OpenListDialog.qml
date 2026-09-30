// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// "Open list": each engine's lists under its name, saved ones first; one
// opened into a tab.
ApplicationWindow {
    id: dialog
    objectName: "bench-open-list"

    required property QuickOpenList openList
    property int chosenGroup: -1
    property int chosenRow: -1

    title: qsTr("Open list")
    width: 480
    height: 420
    visible: true
    color: palette.window

    Component.onDestruction: openList.release()
    onClosing: Qt.callLater(() => dialog.destroy())

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: dialog.close()
    }

    Connections {
        target: dialog.openList
        function onOpened() {
            dialog.close();
        }
    }

    function open() {
        if (chosenGroup >= 0 && chosenRow >= 0)
            openList.open(chosenGroup, chosenRow);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.margin
            spacing: Theme.gap
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: palette.base
                border.color: Theme.hairline(palette)
                radius: 4
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    Column {
                        objectName: "bench-open-list-tree"
                        width: dialog.width - 26
                        Repeater {
                            model: dialog.openList.groups
                            Column {
                                id: group
                                required property var modelData
                                required property int index
                                width: parent.width
                                Label {
                                    leftPadding: 8
                                    topPadding: 6
                                    bottomPadding: 4
                                    text: group.modelData.name
                                    font.bold: true
                                }
                                Repeater {
                                    model: group.modelData.lists
                                    ItemDelegate {
                                        id: listItem
                                        required property var modelData
                                        required property int index
                                        width: group.width
                                        leftPadding: 24
                                        highlighted: dialog.chosenGroup === group.index && dialog.chosenRow === index
                                        onClicked: {
                                            dialog.chosenGroup = group.index;
                                            dialog.chosenRow = index;
                                        }
                                        onDoubleClicked: {
                                            onClicked();
                                            dialog.open();
                                        }
                                        contentItem: RowLayout {
                                            Label {
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                                text: listItem.modelData.label
                                            }
                                            Label {
                                                text: listItem.modelData.tracks
                                                opacity: 0.7
                                            }
                                        }
                                    }
                                }
                                Label {
                                    visible: group.modelData.note !== ""
                                    leftPadding: 24
                                    bottomPadding: 4
                                    opacity: 0.6
                                    text: group.modelData.note
                                }
                            }
                        }
                    }
                }
            }
        }
        WindowFooter {
            Item {
                Layout.fillWidth: true
            }
            DialogButtonBox {
                padding: 0
                standardButtons: DialogButtonBox.Open | DialogButtonBox.Cancel
                onAccepted: dialog.open()
                onRejected: dialog.close()
            }
        }
    }
}
