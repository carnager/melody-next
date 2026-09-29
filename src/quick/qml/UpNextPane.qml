// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    property var dragGhost
    signal closeRequested

    color: Theme.panel
    clip: true

    Rectangle {
        width: 1
        height: parent.height
        color: Theme.line
    }

    // Library rows and list tracks dropped here wait up next; its own rows
    // move. Only the current engine's: up-next is where it plays.
    DropArea {
        id: upNextDrop
        anchors.fill: parent
        keys: ["trackknife"]

        property int insertRow: -1

        function place(y) {
            const local = mapToItem(queue, 0, y).y + queue.contentY;
            const row = queue.indexAt(queue.width / 2, local);
            if (row < 0) {
                insertRow = local < 0 ? 0 : queue.count;
                return;
            }
            const item = queue.itemAtIndex(row);
            insertRow = item !== null && local - item.y > item.height / 2 ? row + 1 : row;
        }

        onEntered: drag => {
            const payload = drag.source.payload;
            drag.accepted = payload !== null && payload.session === Engine.current
                && ["library", "tracks", "upnext"].indexOf(payload.kind) >= 0;
            if (drag.accepted)
                place(drag.y);
        }
        onPositionChanged: drag => place(drag.y)
        onExited: insertRow = -1
        onDropped: drop => {
            const payload = drop.source.payload;
            if (payload.kind === "library")
                payload.session.library.enqueue(payload.row);
            else if (payload.kind === "tracks")
                Engine.tracks.enqueue(payload.rows);
            else if (payload.kind === "upnext") {
                const to = insertRow > payload.row ? insertRow - 1 : insertRow;
                Engine.current.upNext.move(payload.row, Math.max(0, Math.min(to, queue.count - 1)));
            }
            insertRow = -1;
            drop.accept();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.accent
        opacity: upNextDrop.containsDrag ? 0.06 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
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
            acceptedButtons: Qt.NoButton
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
                DragHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onActiveChanged: {
                        if (active)
                            pane.dragGhost.begin({ kind: "upnext", session: Engine.current, row: item.index },
                                                 item.title, centroid.scenePosition);
                        else
                            pane.dragGhost.end();
                    }
                    onCentroidChanged: {
                        if (active)
                            pane.dragGhost.moveTo(centroid.scenePosition);
                    }
                }
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
                visible: queue.count === 0 && !upNextDrop.containsDrag
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

    Rectangle {
        visible: upNextDrop.containsDrag && upNextDrop.insertRow >= 0 && queue.count > 0
        x: queue.mapToItem(pane, 0, 0).x
        width: queue.width
        height: 2
        radius: 1
        color: Theme.accent
        y: {
            const row = upNextDrop.insertRow;
            const item = queue.itemAtIndex(Math.min(row, queue.count - 1));
            if (item === null)
                return queue.mapToItem(pane, 0, 0).y;
            const top = row >= queue.count ? item.y + item.height : item.y;
            return queue.mapToItem(pane, 0, top - queue.contentY).y - 1;
        }
    }
}
