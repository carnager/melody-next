// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// The files' front cover beside their fields: drop an image on it, paste
// one, or fetch one; its menu chooses a file, removes the cover or opens
// the Artwork page.
ColumnLayout {
    id: cover

    required property QuickArtwork artwork
    readonly property var state: artwork.state

    signal openArtwork()
    signal coverSettings()
    signal fetch()

    spacing: 6

    Rectangle {
        id: frame
        Layout.preferredWidth: 150
        Layout.preferredHeight: 150
        color: Shade.mix(palette.base, palette.window, 0.5)
        border.color: drop.containsDrag ? palette.highlight
                                        : Theme.hairline(palette)
        border.width: drop.containsDrag ? 2 : 1
        radius: 4
        activeFocusOnTab: true

        Image {
            anchors.fill: parent
            anchors.margins: 4
            fillMode: Image.PreserveAspectFit
            visible: cover.state.hasFront ?? false
            cache: false
            source: (cover.state.hasFront ?? false)
                    ? "image://artwork/%1/front?%2".arg(cover.artwork.id).arg(cover.artwork.revision)
                    : ""
        }
        Label {
            anchors.centerIn: parent
            width: parent.width - 16
            visible: !(cover.state.hasFront ?? false) || (cover.state.frontMixed ?? false)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: (cover.state.frontMixed ?? false) ? qsTr("Various covers")
                  : ((cover.state.frontEditable ?? false) ? qsTr("No cover\nDrop an image here")
                                                          : qsTr("No cover"))
        }
        DropArea {
            id: drop
            anchors.fill: parent
            enabled: cover.state.frontEditable ?? false
            keys: ["text/uri-list"]
            onDropped: event => {
                if (event.hasUrls && event.urls.length > 0) {
                    cover.artwork.stageFrontCover(event.urls[0]);
                    event.accept(Qt.CopyAction);
                }
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: coverMenu.popup()
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: frame.forceActiveFocus()
        }
        Keys.onPressed: event => {
            if (event.matches(StandardKey.Paste) && (cover.state.frontEditable ?? false)) {
                cover.artwork.pasteFrontCover();
                event.accepted = true;
            }
        }
        ToolTip.visible: coverHover.hovered
        ToolTip.delay: 900
        ToolTip.text: qsTr("Drop or paste an image to make it the front cover; right-click for more")
        HoverHandler {
            id: coverHover
        }
    }
    Button {
        objectName: "bench-metadata-cover-fetch"
        Layout.preferredWidth: 150
        text: qsTr("Fetch cover")
        enabled: cover.state.canFetch ?? false
        onClicked: cover.fetch()
    }
    Item {
        Layout.fillHeight: true
    }

    Menu {
        id: coverMenu
        MenuItem {
            text: qsTr("Fetch cover")
            enabled: cover.state.canFetch ?? false
            onTriggered: cover.fetch()
        }
        MenuItem {
            text: qsTr("Choose file…")
            enabled: cover.state.frontEditable ?? false
            onTriggered: chooseFile.open()
        }
        MenuItem {
            text: qsTr("Paste")
            enabled: cover.state.frontEditable ?? false
            onTriggered: cover.artwork.pasteFrontCover()
        }
        MenuItem {
            text: qsTr("Remove")
            enabled: cover.state.canRemoveFront ?? false
            onTriggered: cover.artwork.removeFrontCover()
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Open Artwork tab")
            onTriggered: cover.openArtwork()
        }
        MenuItem {
            text: qsTr("Cover settings…")
            onTriggered: cover.coverSettings()
        }
    }
    FileDialog {
        id: chooseFile
        title: qsTr("Choose front cover")
        nameFilters: [qsTr("Artwork images (*.png *.jpg *.jpeg)")]
        onAccepted: cover.artwork.stageFrontCover(selectedFile)
    }
}
