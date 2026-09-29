// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    property int source: 1
    property var expanded: ({})
    property int expandedRevision: 0
    readonly property alias searchField: search

    readonly property var results: {
        const q = search.text.trim().toLowerCase();
        const out = [];
        for (let i = 0; i < MockData.albums.length; ++i) {
            const album = MockData.albums[i];
            if (q === "" || album.title.toLowerCase().includes(q) || album.artist.toLowerCase().includes(q))
                out.push(i);
        }
        return out;
    }

    function toggle(a) {
        expanded[a] = !expanded[a];
        ++expandedRevision;
    }

    color: Theme.panel

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // Folders | engine library
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 28
            radius: 5
            color: Theme.window

            Row {
                anchors.fill: parent
                anchors.margins: 2
                Repeater {
                    model: ["Folders", "gemenon"]
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        width: parent.width / 2
                        height: parent.height
                        radius: 4
                        color: pane.source === index ? Theme.raised : "transparent"
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Text {
                            anchors.centerIn: parent
                            text: parent.modelData
                            color: pane.source === parent.index ? Theme.text : Theme.dim
                            font.pixelSize: Theme.fontSize
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: pane.source = parent.index
                        }
                    }
                }
            }
        }

        TextField {
            id: search
            Layout.fillWidth: true
            visible: pane.source === 1
            placeholderText: "Search albums and tracks"
            color: Theme.text
            placeholderTextColor: Theme.faint
            font.pixelSize: Theme.fontSize
            leftPadding: 28
            background: Rectangle {
                radius: 5
                color: Theme.window
                border.color: search.activeFocus ? Theme.accent : Theme.line
            }
            Icon {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                name: "search"
                color: Theme.faint
            }
        }

        ListView {
            id: albums
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: pane.source === 1
            clip: true
            model: pane.results
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Column {
                id: entry
                required property int modelData
                readonly property var album: MockData.albums[modelData]
                readonly property bool open: pane.expandedRevision >= 0 && !!pane.expanded[modelData]

                width: ListView.view.width

                Rectangle {
                    width: parent.width
                    height: 40
                    radius: 4
                    color: hover.hovered ? Theme.hover : "transparent"

                    HoverHandler { id: hover }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: pane.toggle(entry.modelData)
                        onDoubleClicked: App.appendAlbum(entry.modelData)
                    }

                    Icon {
                        x: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 12
                        name: "chevron"
                        color: Theme.faint
                        rotation: entry.open ? 90 : 0
                        Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    }
                    Cover {
                        x: 18
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30
                        height: 30
                        album: entry.album
                    }
                    Column {
                        x: 56
                        width: parent.width - x - 36
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            width: parent.width
                            text: entry.album.title
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: entry.album.artist + " · " + entry.album.tracks.length + " tracks"
                            color: Theme.dim
                            font.pixelSize: Theme.smallFontSize
                            elide: Text.ElideRight
                        }
                    }
                    IconButton {
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "queue"
                        tip: "Add to Up Next"
                        opacity: hover.hovered ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 100 } }
                        onClicked: App.enqueueAlbum(entry.modelData)
                    }
                }

                Repeater {
                    model: entry.open ? entry.album.tracks : []
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: entry.width
                        height: 22
                        radius: 3
                        color: trackHover.hovered ? Theme.hover : "transparent"
                        HoverHandler { id: trackHover }
                        Text {
                            x: 56
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - x - 50
                            text: parent.modelData.n + "  " + parent.modelData.title
                            color: Theme.text
                            font.pixelSize: Theme.smallFontSize + 1
                            elide: Text.ElideRight
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: MockData.duration(parent.modelData.len)
                            color: Theme.dim
                            font.pixelSize: Theme.smallFontSize
                        }
                        MouseArea {
                            anchors.fill: parent
                            onDoubleClicked: App.enqueue(entry.modelData, parent.index)
                        }
                    }
                }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: pane.source === 0
            clip: true
            model: MockData.folders
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                height: 26
                radius: 4
                color: folderHover.hovered ? Theme.hover : "transparent"
                HoverHandler { id: folderHover }
                Icon {
                    x: 6 + parent.modelData.depth * 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    name: "folder"
                    color: Theme.dim
                }
                Text {
                    x: 28 + parent.modelData.depth * 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: parent.modelData.name
                    color: Theme.text
                    font.pixelSize: Theme.fontSize
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: pane.source === 1 ? pane.results.length + " albums" : "Folders on gemenon"
            color: Theme.faint
            font.pixelSize: Theme.smallFontSize
        }
    }
}
