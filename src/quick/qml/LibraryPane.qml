// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    property var dragGhost
    readonly property alias searchField: search
    // Whose library shows: any engine's, whichever tab is open.
    readonly property var session: Engine.sessionByKey(remembered.engine) ?? Engine.sessions[0]
    readonly property var library: session.library
    readonly property var folders: session.folders
    readonly property bool byFolder: remembered.view === "folders"

    Settings {
        id: remembered
        // Its own file: the widgets window's settings are not this one's to write.
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/trackknife/quick-window.conf"
        category: "QuickWindow"
        property string engine: "local"
        property bool queryMode: false
        property int order: 0
        // "albums" or "folders".
        property string view: "albums"
    }

    // Every engine's library searches and sorts alike.
    Binding {
        target: pane.library
        property: "queryMode"
        value: remembered.queryMode
    }
    Binding {
        target: pane.library
        property: "order"
        value: remembered.order
    }

    color: Theme.panel

    Menu {
        id: orderMenu
        MenuItem {
            text: "By artist"
            checkable: true
            checked: remembered.order === 0
            onTriggered: remembered.order = 0
        }
        MenuItem {
            text: "Recently added"
            checkable: true
            checked: remembered.order === 1
            onTriggered: remembered.order = 1
        }
        MenuItem {
            text: "At random"
            checkable: true
            checked: remembered.order === 2
            onTriggered: remembered.order = 2
        }
        MenuSeparator {}
        MenuItem {
            text: remembered.order === 2 ? "Pick again" : "Reload"
            onTriggered: pane.library.refresh()
        }
    }

    Menu {
        id: folderMenu
        property int row: -1
        MenuItem {
            text: "Add to Up Next"
            onTriggered: pane.folders.enqueue(folderMenu.row)
        }
        MenuItem {
            text: "Add to the list on show"
            enabled: Engine.tracks.session === pane.session && Engine.tracks.listId !== ""
            onTriggered: pane.folders.addToList(folderMenu.row, Engine.tracks.listId, -1)
        }
    }

    Menu {
        id: rowMenu
        property int row: -1
        MenuItem {
            text: "Add to Up Next"
            onTriggered: pane.library.enqueue(rowMenu.row)
        }
        MenuItem {
            text: "Add to the list on show"
            enabled: Engine.tracks.session === pane.session && Engine.tracks.listId !== ""
            onTriggered: pane.library.addToList(rowMenu.row, Engine.tracks.listId, -1)
        }
    }

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

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            TextField {
                id: search
                Layout.fillWidth: true
                enabled: !pane.byFolder
                opacity: enabled ? 1 : 0.5
                placeholderText: remembered.queryMode ? "Query, e.g. artist HAS wilson AND rating GREATER 6"
                                                      : "Search albums and tracks"
                color: Theme.text
                placeholderTextColor: Theme.faint
                font.pixelSize: Theme.fontSize
                font.family: remembered.queryMode ? "monospace" : Qt.application.font.family
                leftPadding: 28
                onTextChanged: pane.library.search = text
                Keys.onEscapePressed: text = ""
                background: Rectangle {
                    radius: 5
                    color: Theme.window
                    border.color: pane.library.error !== "" ? Theme.error
                                : search.activeFocus ? Theme.accent : Theme.line
                }
                Icon {
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    name: remembered.queryMode ? "query" : "search"
                    color: Theme.faint
                }
            }
            IconButton {
                icon: "query"
                checked: remembered.queryMode
                tip: remembered.queryMode ? "Searching with a query; click for words" : "Search with a query"
                onClicked: remembered.queryMode = !remembered.queryMode
            }
            IconButton {
                id: orderButton
                icon: "sort"
                tip: "Order"
                visible: !pane.byFolder
                onClicked: orderMenu.popup(orderButton, 0, orderButton.height)
            }
            IconButton {
                icon: "folder"
                checked: pane.byFolder
                tip: pane.byFolder ? "Browsing by folder; click for albums" : "Browse by folder"
                onClicked: {
                    remembered.view = pane.byFolder ? "albums" : "folders";
                    if (pane.byFolder && pane.folders.atTop)
                        pane.folders.refresh();
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: pane.library.error !== ""
            text: pane.library.error
            wrapMode: Text.WordWrap
            color: Theme.error
            font.pixelSize: Theme.smallFontSize
        }

        ListView {
            id: albums
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !pane.byFolder
            clip: true
            model: pane.library
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            // A mouse drag drags a row out; the wheel and touch still scroll.
            acceptedButtons: Qt.NoButton
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: entry
                required property int index
                required property string kind
                required property string title
                required property string artist
                required property string album
                required property string date
                required property int tracks
                required property int number
                required property real duration
                required property bool expanded
                required property string cover

                readonly property bool isAlbum: kind === "album"
                readonly property bool isSection: kind === "section"

                width: ListView.view.width
                height: isSection ? 26 : kind === "child" ? 22 : 40
                radius: 4
                color: hover.hovered && !isSection ? Theme.hover : "transparent"

                HoverHandler { id: hover }
                DragHandler {
                    target: null
                    enabled: !entry.isSection
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onActiveChanged: {
                        if (active)
                            pane.dragGhost.begin({ kind: "library", session: pane.session, source: pane.library,
                                                   row: entry.index },
                                                 entry.isAlbum ? entry.title + " — " + entry.artist : entry.title,
                                                 centroid.scenePosition);
                        else
                            pane.dragGhost.end();
                    }
                    onCentroidChanged: {
                        if (active)
                            pane.dragGhost.moveTo(centroid.scenePosition);
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !entry.isSection
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            rowMenu.row = entry.index;
                            rowMenu.popup();
                        } else if (entry.isAlbum) {
                            pane.library.toggle(entry.index);
                        }
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton && !entry.isAlbum)
                            pane.library.enqueue(entry.index);
                    }
                }

                // Section heading
                Text {
                    visible: entry.isSection
                    x: 4
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: entry.title
                    color: Theme.faint
                    font.pixelSize: Theme.smallFontSize
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.6
                }

                // Album, or a track found on its own
                Icon {
                    visible: entry.isAlbum
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
                    visible: entry.isAlbum || entry.kind === "track"
                    x: 18
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30
                    height: 30
                    source: visible ? entry.cover : ""
                    name: entry.isAlbum ? entry.title : entry.album
                }
                Column {
                    visible: entry.isAlbum || entry.kind === "track"
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
                        text: entry.isAlbum
                              ? entry.artist + " · " + entry.tracks + (entry.tracks === 1 ? " track" : " tracks")
                                + (entry.date !== "" ? " · " + entry.date : "")
                              : entry.artist + (entry.album !== "" ? " · " + entry.album : "")
                                + (entry.duration > 0 ? " · " + Engine.formatDuration(entry.duration) : "")
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize
                        elide: Text.ElideRight
                    }
                }

                // A track under its expanded album
                Text {
                    visible: entry.kind === "child"
                    x: 56
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - 76
                    text: (entry.number > 0 ? entry.number + "  " : "") + entry.title
                    color: Theme.text
                    font.pixelSize: Theme.smallFontSize + 1
                    elide: Text.ElideRight
                }
                Text {
                    visible: entry.kind === "child" && !hover.hovered && entry.duration > 0
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: Engine.formatDuration(entry.duration)
                    color: Theme.dim
                    font.pixelSize: Theme.smallFontSize
                }

                IconButton {
                    visible: !entry.isSection
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    icon: "queue"
                    iconSize: entry.kind === "child" ? 12 : 16
                    tip: "Add to Up Next"
                    opacity: hover.hovered ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 100 } }
                    onClicked: pane.library.enqueue(entry.index)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: albums.count === 0 && !pane.library.loading && pane.library.error === ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: !pane.session.connected ? "Connecting to " + pane.session.name + "…\n" + pane.session.failure
                    : search.text !== "" ? "Nothing matches “" + search.text + "”"
                    : "The library of " + pane.session.name + " is empty"
                color: Theme.dim
                font.pixelSize: Theme.fontSize
            }
        }

        // By folder
        Rectangle {
            Layout.fillWidth: true
            visible: pane.byFolder
            implicitHeight: 30
            radius: 4
            color: Theme.window
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 2
                anchors.rightMargin: 8
                IconButton {
                    icon: "chevron"
                    rotation: 180
                    iconSize: 12
                    tip: "Up"
                    enabled: !pane.folders.atTop
                    opacity: enabled ? 1 : 0.35
                    onClicked: pane.folders.up()
                }
                Text {
                    Layout.fillWidth: true
                    text: pane.folders.atTop ? "Library folders" : pane.folders.name
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.pixelSize: Theme.fontSize
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: pane.byFolder && pane.folders.error !== ""
            text: pane.folders.error
            wrapMode: Text.WordWrap
            color: Theme.error
            font.pixelSize: Theme.smallFontSize
        }
        ListView {
            id: folderView
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: pane.byFolder
            clip: true
            model: pane.folders
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            acceptedButtons: Qt.NoButton
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: node
                required property int index
                required property string kind
                required property string title
                required property string artist
                required property string album
                required property int number
                required property real duration
                required property string cover
                readonly property bool isFolder: kind === "folder"

                width: ListView.view.width
                height: isFolder ? 28 : 36
                radius: 4
                color: nodeHover.hovered ? Theme.hover : "transparent"

                HoverHandler { id: nodeHover }
                DragHandler {
                    target: null
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onActiveChanged: {
                        if (active)
                            pane.dragGhost.begin({ kind: "library", session: pane.session, source: pane.folders,
                                                   row: node.index },
                                                 node.title, centroid.scenePosition);
                        else
                            pane.dragGhost.end();
                    }
                    onCentroidChanged: {
                        if (active)
                            pane.dragGhost.moveTo(centroid.scenePosition);
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        if (mouse.button === Qt.RightButton) {
                            folderMenu.row = node.index;
                            folderMenu.popup();
                        } else if (node.isFolder) {
                            pane.folders.open(node.index);
                        }
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button === Qt.LeftButton && !node.isFolder)
                            pane.folders.enqueue(node.index);
                    }
                }
                Icon {
                    visible: node.isFolder
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    name: "folder"
                    color: Theme.dim
                }
                Cover {
                    visible: !node.isFolder
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 26
                    height: 26
                    source: visible ? node.cover : ""
                    name: node.album
                }
                Column {
                    x: node.isFolder ? 32 : 40
                    width: parent.width - x - 36
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        text: (!node.isFolder && node.number > 0 ? node.number + "  " : "") + node.title
                        elide: node.isFolder ? Text.ElideMiddle : Text.ElideRight
                        color: Theme.text
                        font.pixelSize: Theme.fontSize
                    }
                    Text {
                        visible: !node.isFolder
                        width: parent.width
                        text: node.artist + (node.duration > 0 ? " · " + Engine.formatDuration(node.duration) : "")
                        elide: Text.ElideRight
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize
                    }
                }
                IconButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    icon: "queue"
                    iconSize: 14
                    tip: node.isFolder ? "Add everything in it to Up Next" : "Add to Up Next"
                    opacity: nodeHover.hovered ? 1 : 0
                    onClicked: pane.folders.enqueue(node.index)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: folderView.count === 0 && !pane.folders.loading && pane.folders.error === ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: pane.folders.atTop ? "No library folders on " + pane.session.name : "Nothing indexed here"
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
