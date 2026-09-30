// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    color: Theme.panel
    clip: true

    Rectangle {
        width: 1
        height: parent.height
        color: Theme.line
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Column {
                Layout.fillWidth: true
                Text {
                    text: "Up Next"
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    text: "This computer · " + App.queue.count + " waiting"
                    color: Theme.dim
                    font.pixelSize: Theme.smallFontSize
                }
            }
            IconButton {
                icon: "close"
                iconSize: 12
                tip: "Hide Up Next"
                onClicked: App.upNextOpen = false
            }
        }

        ListView {
            id: queue
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: App.queue
            ScrollBar.vertical: ScrollBar {}

            add: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
                NumberAnimation { property: "x"; from: 24; to: 0; duration: 220; easing.type: Easing.OutCubic }
            }
            remove: Transition {
                NumberAnimation { property: "opacity"; to: 0; duration: 150 }
                NumberAnimation { property: "x"; to: -24; duration: 150 }
            }
            displaced: Transition {
                NumberAnimation { properties: "x,y"; duration: 200; easing.type: Easing.OutCubic }
            }

            delegate: Rectangle {
                id: entry
                required property int index
                required property int a
                required property int t
                readonly property var album: MockData.albums[a]
                readonly property var track: album.tracks[t]

                width: ListView.view.width
                height: 42
                radius: 4
                color: entryHover.hovered ? Theme.hover : "transparent"

                HoverHandler { id: entryHover }
                MouseArea {
                    anchors.fill: parent
                    onDoubleClicked: {
                        const a = entry.a, t = entry.t;
                        App.queue.remove(entry.index);
                        App.play(a, t);
                    }
                }
                Cover {
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 32
                    album: entry.album
                }
                Column {
                    x: 44
                    width: parent.width - x - 30
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: entry.track.title
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: entry.album.artist + " · " + MockData.duration(entry.track.len)
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                }
                IconButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    icon: "close"
                    iconSize: 11
                    tip: "Remove"
                    opacity: entryHover.hovered ? 1 : 0
                    onClicked: App.queue.remove(entry.index)
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: App.queue.count === 0
                spacing: 6
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "Nothing waiting"
                    color: Theme.text
                    font.pixelSize: 15
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: "Add albums from the library, or double-click a track in an expanded album."
                    color: Theme.dim
                    font.pixelSize: Theme.fontSize
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                text: "Clear"
                enabled: App.queue.count > 0
                flat: true
                font.pixelSize: Theme.smallFontSize
                palette.buttonText: Theme.dim
                onClicked: App.queue.clear()
            }
        }
    }
}
