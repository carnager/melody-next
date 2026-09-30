// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick

// The library's folders: which belong to it, added and removed; saved at
// once, the files never touched. An engine elsewhere's are typed as that
// machine sees them.
Dialog {
    id: dialog
    objectName: "local-library-folders-dialog"

    property var browser

    title: browser && browser.remote() ? qsTr("Library folders on %1").arg(browser.name())
                                       : qsTr("Local library folders")
    anchors.centerIn: Overlay.overlay
    width: 560
    height: 320
    modal: false
    standardButtons: Dialog.Close

    onOpened: if (browser) browser.loadRoots()

    ColumnLayout {
        anchors.fill: parent
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.browser && dialog.browser.remote()
                  ? qsTr("Folders on %1 for its engine to index. Give each path as that machine sees it. Folder changes are saved immediately. Removing a folder leaves its files untouched.").arg(dialog.browser.name())
                  : qsTr("Choose the folders to browse and search as your local music library. Folder changes are saved immediately. Removing a folder leaves its files untouched. ")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Only Refresh in the Library sidebar scans your folders for music.")
        }
        ListView {
            id: roots
            objectName: "local-library-roots"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.browser ? dialog.browser.roots : []
            currentIndex: -1
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: roots.width
                text: modelData.label
                highlighted: ListView.isCurrentItem
                ToolTip.visible: hovered && modelData.tooltip !== ""
                ToolTip.text: modelData.tooltip
                onClicked: roots.currentIndex = index
            }
        }
        Label {
            objectName: "local-library-folder-error"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dialog.browser ? dialog.browser.rootsError : ""
        }
        RowLayout {
            Button {
                objectName: "local-library-folder-add"
                text: qsTr("Add folder…")
                onClicked: {
                    if (dialog.browser.remote())
                        remotePath.open();
                    else
                        folderPicker.open();
                }
            }
            Button {
                objectName: "local-library-folder-remove"
                text: qsTr("Remove")
                enabled: roots.currentIndex >= 0
                onClicked: {
                    dialog.browser.removeRoot(roots.currentIndex);
                    roots.currentIndex = -1;
                }
            }
        }
    }

    FolderDialog {
        id: folderPicker
        title: qsTr("Add music folder")
        onAccepted: dialog.browser.addRoot(decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, "")))
    }

    // ADR-0227: a folder on the remote machine cannot be browsed from here:
    // the path is typed as that machine sees it, and the engine checks it.
    Dialog {
        id: remotePath
        title: qsTr("Add music folder")
        anchors.centerIn: Overlay.overlay
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            Label {
                text: dialog.browser ? qsTr("Folder on %1, as that machine sees it:").arg(dialog.browser.name()) : ""
            }
            TextField {
                id: remoteField
                Layout.preferredWidth: 360
            }
        }
        onOpened: {
            remoteField.text = "";
            remoteField.forceActiveFocus();
        }
        onAccepted: dialog.browser.addRoot(remoteField.text)
    }
}
