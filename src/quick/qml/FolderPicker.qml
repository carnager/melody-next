// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// Chooses a folder on an engine's machine -- not this one's: the engine
// lists them (folders.list), so a NAS's folders are the NAS's.
Dialog {
    id: picker

    property var session: null
    property var action: null
    property var listing: ({})

    function choose(session, action) {
        picker.session = session;
        picker.action = action;
        picker.listing = {};
        open();
        session.listFolders("");
    }

    title: session ? "A folder on " + session.name : ""
    modal: true
    anchors.centerIn: Overlay.overlay
    width: 520
    height: 480
    standardButtons: Dialog.Cancel

    background: Rectangle {
        color: Theme.panel
        radius: 8
        border.color: Theme.line
    }

    Connections {
        target: picker.session
        enabled: picker.opened
        function onFoldersListed(listing) {
            picker.listing = listing;
        }
    }

    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            IconButton {
                icon: "chevron"
                rotation: 180
                iconSize: 12
                tip: "Up"
                enabled: (picker.listing.parent ?? "") !== ""
                opacity: enabled ? 1 : 0.4
                onClicked: picker.session.listFolders(picker.listing.parent)
            }
            Text {
                Layout.fillWidth: true
                text: picker.listing.error ?? picker.listing.name ?? ""
                elide: Text.ElideMiddle
                color: picker.listing.error ? Theme.error : Theme.text
                font.pixelSize: Theme.fontSize
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.base
            radius: 5
            border.color: Theme.line
            ListView {
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: picker.listing.folders ?? []
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: 26
                    radius: 4
                    color: folderMouse.containsMouse ? Theme.hover : "transparent"
                    Icon {
                        x: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 15
                        height: 15
                        name: "folder"
                        color: Theme.dim
                    }
                    Text {
                        x: 28
                        width: parent.width - x - 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: parent.modelData.name
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                    }
                    MouseArea {
                        id: folderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: picker.session.listFolders(parent.modelData.path)
                    }
                }
            }
        }
        Button {
            Layout.alignment: Qt.AlignRight
            text: "Use this folder"
            enabled: (picker.listing.path ?? "") !== ""
            onClicked: {
                if (picker.action)
                    picker.action(picker.listing.path);
                picker.close();
            }
        }
    }
}
