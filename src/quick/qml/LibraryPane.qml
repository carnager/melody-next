// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    readonly property alias searchField: search
    // Whose library shows: any engine's, whichever tab is open.
    readonly property var session: Engine.sessionByKey(remembered.engine) ?? Engine.sessions[0]
    readonly property var library: session.library

    Settings {
        id: remembered
        // Its own file: the widgets window's settings are not this one's to write.
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/trackknife/quick-window.conf"
        category: "QuickWindow"
        property string engine: "local"
    }

    color: Theme.panel

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // One segment per engine.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 28
            radius: 5
            color: Theme.window

            Row {
                anchors.fill: parent
                anchors.margins: 2
                Repeater {
                    model: Engine.sessions
                    delegate: Rectangle {
                        id: segment
                        required property var modelData
                        readonly property bool chosen: pane.session === modelData
                        width: parent.width / Engine.sessions.length
                        height: parent.height
                        radius: 4
                        color: chosen ? Theme.raised : "transparent"
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Row {
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 6
                                height: 6
                                radius: 3
                                color: segment.modelData.connected ? "#6cc28b" : Theme.faint
                            }
                            Text {
                                text: segment.modelData.name
                                color: segment.chosen ? Theme.text : Theme.dim
                                font.pixelSize: Theme.fontSize
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                remembered.engine = segment.modelData.key;
                                search.text = pane.library.search;
                            }
                        }
                    }
                }
            }
        }

        TextField {
            id: search
            Layout.fillWidth: true
            placeholderText: "Search albums and artists"
            color: Theme.text
            placeholderTextColor: Theme.faint
            font.pixelSize: Theme.fontSize
            leftPadding: 28
            onTextChanged: pane.library.search = text
            Keys.onEscapePressed: text = ""
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
            clip: true
            model: pane.library
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: entry
                required property int index
                required property bool track
                required property string title
                required property string artist
                required property string date
                required property int tracks
                required property int number
                required property real duration
                required property bool expanded
                required property string cover

                width: ListView.view.width
                height: track ? 22 : 40
                radius: 4
                color: hover.hovered ? Theme.hover : "transparent"

                HoverHandler { id: hover }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (!entry.track)
                            pane.library.toggle(entry.index);
                    }
                    onDoubleClicked: {
                        if (entry.track)
                            pane.library.enqueue(entry.index);
                    }
                }

                // Album
                Icon {
                    visible: !entry.track
                    x: 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    name: "chevron"
                    color: Theme.faint
                    rotation: entry.expanded ? 90 : 0
                    Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
                Cover {
                    visible: !entry.track
                    x: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 30
                    source: entry.track ? "" : entry.cover
                    name: entry.title
                }
                Column {
                    visible: !entry.track
                    x: 56
                    width: parent.width - x - 36
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: entry.title
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: entry.artist + " · " + entry.tracks + (entry.tracks === 1 ? " track" : " tracks")
                              + (entry.date !== "" ? " · " + entry.date : "")
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                }

                // Track
                Text {
                    visible: entry.track
                    x: 56
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - 76
                    text: (entry.number > 0 ? entry.number + "  " : "") + entry.title
                    color: Theme.text
                    font.pixelSize: Theme.smallFontSize + 1
                    elide: Text.ElideRight
                }
                Text {
                    visible: entry.track && !hover.hovered
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: Engine.formatDuration(entry.duration)
                    color: Theme.dim
                    font.pixelSize: Theme.smallFontSize
                }

                IconButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    icon: "queue"
                    iconSize: entry.track ? 12 : 16
                    tip: "Add to Up Next"
                    opacity: hover.hovered ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    onClicked: pane.library.enqueue(entry.index)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: albums.count === 0 && !pane.library.loading
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: !pane.session.connected ? "Connecting to " + pane.session.name + "…\n" + pane.session.failure
                    : search.text !== "" ? "Nothing matches “" + search.text + "”"
                    : "The library of " + pane.session.name + " is empty"
                color: Theme.dim
                font.pixelSize: Theme.fontSize
            }
        }

        Text {
            Layout.fillWidth: true
            visible: pane.library.loading
            text: "Loading…"
            color: Theme.faint
            font.pixelSize: Theme.smallFontSize
        }
    }
}
