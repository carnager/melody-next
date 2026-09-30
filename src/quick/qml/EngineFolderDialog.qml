// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// ADR-0237: a folder on an engine's machine, as that engine lists it --
// double-click to go in, Up to go out, Choose for the folder shown or the
// one selected in it.
ApplicationWindow {
    id: chooser
    objectName: "bench-engine-folder-dialog"

    required property QuickEngineFolder folder
    readonly property var state: folder.state

    title: folder.title
    width: 520
    height: 420
    visible: true
    color: palette.window
    modality: Qt.ApplicationModal

    Component.onDestruction: folder.release()
    onClosing: Qt.callLater(() => chooser.destroy())

    // Esc closes it, as it closes any dialog.
    Shortcut {
        sequence: StandardKey.Cancel
        onActivated: chooser.close()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8
        RowLayout {
            Button {
                objectName: "bench-engine-folder-up"
                text: qsTr("Up")
                enabled: chooser.state.canUp ?? false
                onClicked: {
                    folders.currentIndex = -1;
                    chooser.folder.up();
                }
            }
            Label {
                objectName: "bench-engine-folder-path"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: chooser.state.path ?? ""
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: palette.base
            border.color: Theme.hairline(palette)
            radius: 4
            ListView {
                id: folders
                objectName: "bench-engine-folder-list"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                currentIndex: -1
                model: chooser.state.folders ?? []
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: folders.width
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    onClicked: folders.currentIndex = index
                    onDoubleClicked: {
                        folders.currentIndex = -1;
                        chooser.folder.open(index);
                    }
                }
            }
        }
        Label {
            objectName: "bench-engine-folder-status"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text !== ""
            text: chooser.state.status ?? ""
        }
        DialogButtonBox {
            Layout.fillWidth: true
            Button {
                objectName: "bench-engine-folder-choose"
                text: qsTr("Choose")
                enabled: chooser.state.canChoose ?? false
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            onAccepted: {
                chooser.folder.choose(folders.currentIndex);
                chooser.close();
            }
            onRejected: chooser.close()
        }
    }
}
