// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    signal closeRequested

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
                    text: Engine.current.name + " · " + queue.count + " waiting"
                    color: Theme.dim
                    font.pixelSize: Theme.smallFontSize
                }
            }
            IconButton {
                icon: "close"
                iconSize: 12
                tip: "Hide Up Next"
                onClicked: pane.closeRequested()
            }
        }

        ListView {
            id: queue
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: Engine.current.upNext
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
            move: Transition {
                NumberAnimation { properties: "x,y"; duration: 200; easing.type: Easing.OutCubic }
            }

            delegate: Rectangle {
                id: item
                required property int index
                required property string title
                required property string artist
                required property string album
                required property real duration
                required property string cover

                width: ListView.view.width
                height: 42
                radius: 4
                color: itemHover.hovered ? Theme.hover : "transparent"

                HoverHandler { id: itemHover }
                Cover {
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    height: 32
                    source: item.cover
                    name: item.album
                }
                Column {
                    x: 44
                    width: parent.width - x - (itemHover.hovered ? 80 : 8)
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: item.title
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: item.artist + (item.duration > 0 ? " · " + Engine.formatDuration(item.duration) : "")
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: itemHover.hovered
                    IconButton {
                        icon: "chevron"
                        iconSize: 11
                        rotation: -90
                        tip: "Earlier"
                        visible: item.index > 0
                        onClicked: Engine.current.upNext.move(item.index, item.index - 1)
                    }
                    IconButton {
                        icon: "chevron"
                        iconSize: 11
                        rotation: 90
                        tip: "Later"
                        visible: item.index < queue.count - 1
                        onClicked: Engine.current.upNext.move(item.index, item.index + 1)
                    }
                    IconButton {
                        icon: "close"
                        iconSize: 11
                        tip: "Remove"
                        onClicked: Engine.current.upNext.remove(item.index)
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: queue.count === 0
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
                    text: "Add albums or tracks from the library, or press Q on tracks in a list."
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
                enabled: queue.count > 0
                flat: true
                font.pixelSize: Theme.smallFontSize
                palette.buttonText: Theme.dim
                onClicked: Engine.current.upNext.clear()
            }
        }
    }
}
