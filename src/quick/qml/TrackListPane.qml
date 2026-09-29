// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    readonly property var tracks: Engine.tracks
    property var selected: ({})
    property int selectionRevision: 0
    property int anchorRow: -1

    readonly property int gutter: 66
    readonly property int numberWidth: 26
    readonly property int lengthWidth: 56

    // Which lists show as tabs: every engine's working lists, and the saved
    // ones opened here. Remembered for next time, like the list on show.
    // Lists are named "<engine key>|<list id>" there.
    Settings {
        id: remembered
        // Its own file: the widgets window's settings are not this one's to write.
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/trackknife/quick-window.conf"
        category: "QuickWindow"
        property string currentList: ""
        property var openSaved: []
    }

    function tabKey(session, listId) {
        return session.key + "|" + listId;
    }

    function isTab(session, listId, saved) {
        return !saved || remembered.openSaved.indexOf(tabKey(session, listId)) >= 0;
    }

    function show(session, listId) {
        remembered.currentList = tabKey(session, listId);
        Engine.show(session, listId);
    }

    function openSaved(session, listId) {
        const key = tabKey(session, listId);
        if (remembered.openSaved.indexOf(key) < 0)
            remembered.openSaved = remembered.openSaved.concat([key]);
        show(session, listId);
    }

    function closeSaved(session, listId) {
        const key = tabKey(session, listId);
        remembered.openSaved = remembered.openSaved.filter(k => k !== key);
        if (Engine.tracks.session === session && Engine.tracks.listId === listId)
            show(session, session.lists.firstWorking());
    }

    // When an engine's lists arrive: the remembered list if it is this
    // engine's (its first working list if that one is gone), else this
    // engine's first working list if nothing better shows yet.
    function adoptLists(session) {
        const shown = Engine.tracks.session;
        const separator = remembered.currentList.indexOf("|");
        const rememberedEngine = remembered.currentList.slice(0, separator);
        const rememberedList = remembered.currentList.slice(separator + 1);
        if (rememberedEngine === session.key) {
            Engine.show(session, session.lists.indexOf(rememberedList) >= 0 ? rememberedList
                                                                            : session.lists.firstWorking());
            return;
        }
        const stale = shown === session && session.lists.indexOf(Engine.tracks.listId) < 0;
        if (shown === null || stale)
            Engine.show(session, session.lists.firstWorking());
    }

    Instantiator {
        model: Engine.sessions
        delegate: Connections {
            required property var modelData
            target: modelData.lists
            function onLoaded() {
                pane.adoptLists(modelData);
            }
        }
    }

    Connections {
        target: Engine.tracks
        function onLoaded() {
            pane.clearSelection();
        }
    }

    function selectedRows() {
        return Object.keys(selected).filter(k => selected[k]).map(Number).sort((a, b) => a - b);
    }

    function clearSelection() {
        selected = {};
        anchorRow = -1;
        ++selectionRevision;
    }

    function select(row, modifiers) {
        const extend = modifiers & Qt.ShiftModifier;
        const toggle = modifiers & Qt.ControlModifier;
        if (extend && anchorRow >= 0) {
            if (!toggle)
                selected = {};
            const lo = Math.min(anchorRow, row), hi = Math.max(anchorRow, row);
            for (let i = lo; i <= hi; ++i)
                selected[i] = true;
        } else if (toggle) {
            selected[row] = !selected[row];
            anchorRow = row;
        } else {
            selected = {};
            selected[row] = true;
            anchorRow = row;
        }
        ++selectionRevision;
    }

    function selectAlbum(headerRow) {
        selected = {};
        for (const row of Engine.tracks.groupRows(headerRow))
            selected[row] = true;
        anchorRow = headerRow + 1;
        ++selectionRevision;
    }

    function step(delta, modifiers) {
        const count = view.count;
        let row = anchorRow < 0 ? (delta > 0 ? -1 : count) : anchorRow;
        const keep = anchorRow;
        row += delta;
        // Headers are not selectable on their own.
        if (row >= 0 && row < count && Engine.tracks.groupRows(row).length > 0)
            row += delta;
        if (row < 0 || row >= count)
            return;
        if (modifiers & Qt.ShiftModifier) {
            select(row, Qt.ShiftModifier);
            anchorRow = keep;
        } else {
            select(row, Qt.NoModifier);
        }
        view.positionViewAtIndex(row, ListView.Contain);
    }

    readonly property string summary: selectionRevision >= 0 && Engine.tracks.trackCount >= 0
                                      ? Engine.tracks.describe(selectedRows()) : ""

    color: Theme.base

    Menu {
        id: savedMenu
        Instantiator {
            model: Engine.sessions
            delegate: Menu {
                id: engineMenu
                required property var modelData
                title: modelData.name
                Instantiator {
                    model: engineMenu.modelData.lists
                    delegate: MenuItem {
                        required property string listId
                        required property string name
                        required property bool saved
                        required property int tracks
                        text: name + "  ·  " + tracks
                        visible: saved
                        height: visible ? implicitHeight : 0
                        onTriggered: pane.openSaved(engineMenu.modelData, listId)
                    }
                    onObjectAdded: (index, object) => engineMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => engineMenu.removeItem(object)
                }
            }
            onObjectAdded: (index, object) => savedMenu.insertMenu(index, object)
            onObjectRemoved: (index, object) => savedMenu.removeMenu(object)
        }
    }

    Menu {
        id: rowMenu
        MenuItem {
            text: "Play"
            onTriggered: {
                const rows = pane.selectedRows();
                if (rows.length > 0)
                    Engine.tracks.play(rows[0]);
            }
        }
        MenuItem {
            text: "Add to Up Next"
            onTriggered: Engine.tracks.enqueue(pane.selectedRows())
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Tabs
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            color: Theme.panel

            Flickable {
                anchors.fill: parent
                anchors.leftMargin: 6
                contentWidth: tabRow.width + 40
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                Row {
                    id: tabRow
                    anchors.bottom: parent.bottom
                    spacing: 2

                    Repeater {
                        model: Engine.sessions
                        delegate: Row {
                            id: group
                            required property var modelData
                            readonly property var session: modelData
                            height: 30
                            spacing: 2

                            // The engine's name, where there is more than one.
                            Text {
                                visible: Engine.sessions.length > 1
                                anchors.verticalCenter: parent.verticalCenter
                                leftPadding: 8
                                rightPadding: 4
                                text: group.session.name
                                color: group.session.connected ? Theme.faint : Qt.darker(Theme.faint, 1.4)
                                font.pixelSize: Theme.smallFontSize
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.5
                            }

                            Repeater {
                                model: group.session.lists
                                delegate: Rectangle {
                                    id: tab
                                    required property string listId
                                    required property string name
                                    required property bool saved
                                    readonly property bool current: Engine.tracks.session === group.session
                                                                    && Engine.tracks.listId === listId

                                    visible: pane.isTab(group.session, listId, saved)
                                    width: visible ? Math.min(220, tabLabel.implicitWidth + 50) : 0
                                    height: 30
                                    radius: 5
                                    color: current ? Theme.base : tabHover.hovered ? Theme.hover : "transparent"
                                    Behavior on color { ColorAnimation { duration: 100 } }

                                    // Square off the bottom corners so the tab joins the list.
                                    Rectangle {
                                        visible: tab.current
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 6
                                        color: Theme.base
                                    }
                                    HoverHandler { id: tabHover }
                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                        onClicked: mouse => {
                                            if (mouse.button === Qt.MiddleButton && tab.saved)
                                                pane.closeSaved(group.session, tab.listId);
                                            else
                                                pane.show(group.session, tab.listId);
                                        }
                                    }
                                    Rectangle {
                                        id: kindDot
                                        x: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 7
                                        height: 7
                                        radius: 3.5
                                        color: tab.saved ? Theme.accent : "#6cc28b"
                                    }
                                    Text {
                                        id: tabLabel
                                        anchors.left: kindDot.right
                                        anchors.leftMargin: 7
                                        anchors.right: parent.right
                                        anchors.rightMargin: 26
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: tab.name
                                        elide: Text.ElideRight
                                        color: tab.current ? Theme.text : Theme.dim
                                        font.pixelSize: Theme.fontSize
                                        font.italic: !tab.saved
                                    }
                                    IconButton {
                                        visible: tab.saved
                                        anchors.right: parent.right
                                        anchors.rightMargin: 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        icon: "close"
                                        iconSize: 11
                                        tip: "Close"
                                        opacity: tab.current || tabHover.hovered ? 1 : 0
                                        onClicked: pane.closeSaved(group.session, tab.listId)
                                    }
                                }
                            }
                        }
                    }

                    IconButton {
                        id: openButton
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "plus"
                        iconSize: 13
                        tip: "Open a saved list"
                        onClicked: savedMenu.popup(openButton, 0, openButton.height)
                    }
                }
            }
        }

        // Column header
        Item {
            Layout.fillWidth: true
            implicitHeight: 26

            Text {
                x: pane.gutter
                width: pane.numberWidth
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: "#"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                x: pane.gutter + pane.numberWidth + 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Title"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                x: parent.width - pane.lengthWidth - 12
                width: pane.lengthWidth
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: "Length"
                color: Theme.faint
                font.pixelSize: Theme.smallFontSize
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.line
            }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: Engine.tracks
            reuseItems: true
            cacheBuffer: 400
            boundsBehavior: Flickable.StopAtBounds
            topMargin: 6
            bottomMargin: 12
            ScrollBar.vertical: ScrollBar {}

            Keys.onUpPressed: event => pane.step(-1, event.modifiers)
            Keys.onDownPressed: event => pane.step(1, event.modifiers)
            Keys.onReturnPressed: {
                const rows = pane.selectedRows();
                if (rows.length > 0)
                    Engine.tracks.play(rows[0]);
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Q) {
                    Engine.tracks.enqueue(pane.selectedRows());
                    event.accepted = true;
                } else if (event.key === Qt.Key_Escape) {
                    pane.clearSelection();
                    event.accepted = true;
                }
            }

            delegate: Item {
                id: row
                required property int index
                required property bool header
                required property string entry
                required property string title
                required property string artist
                required property string album
                required property string date
                required property int number
                required property real duration
                required property string cover
                required property int groupTracks
                required property real groupDuration

                readonly property bool isSelected: pane.selectionRevision >= 0 && !!pane.selected[index]
                readonly property bool isPlaying: !header && entry !== "" && entry === Engine.current.player.entry

                width: ListView.view.width
                height: header ? 30 : Theme.rowHeight
                // The album cover hangs into the rows below its header.
                z: header ? 2 : 1

                HoverHandler { id: rowHover }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: mouse => {
                        view.forceActiveFocus();
                        if (mouse.button === Qt.RightButton) {
                            if (!row.isSelected) {
                                if (row.header)
                                    pane.selectAlbum(row.index);
                                else
                                    pane.select(row.index, Qt.NoModifier);
                            }
                            rowMenu.popup();
                            return;
                        }
                        if (row.header)
                            pane.selectAlbum(row.index);
                        else
                            pane.select(row.index, mouse.modifiers);
                    }
                    onDoubleClicked: mouse => {
                        if (mouse.button !== Qt.LeftButton)
                            return;
                        if (row.header) {
                            const rows = Engine.tracks.groupRows(row.index);
                            if (rows.length > 0)
                                Engine.tracks.play(rows[0]);
                        } else {
                            Engine.tracks.play(row.index);
                        }
                    }
                }

                Rectangle {
                    visible: !row.header
                    x: pane.gutter - 6
                    width: parent.width - x - 6
                    height: parent.height
                    radius: 3
                    color: row.isSelected ? Theme.selection : row.isPlaying ? Theme.playingTint
                         : rowHover.hovered ? Theme.hover : "transparent"
                    border.color: row.isSelected || row.isPlaying ? Theme.playingLine : "transparent"
                }

                // Album header
                Cover {
                    visible: row.header
                    x: 12
                    y: 6
                    width: 44
                    height: 44
                    source: row.header ? row.cover : ""
                    name: row.album
                }
                Row {
                    visible: row.header
                    x: pane.gutter
                    width: parent.width - x - 12
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    spacing: 10
                    Text {
                        id: albumTitle
                        width: Math.min(implicitWidth, parent.width * 0.6)
                        text: row.album !== "" ? row.album : "Unknown album"
                        color: Theme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        anchors.baseline: albumTitle.baseline
                        width: parent.width - albumTitle.width - 10
                        text: row.artist + (row.date !== "" ? " · " + row.date : "") + " · " + row.groupTracks
                              + (row.groupTracks === 1 ? " track · " : " tracks · ")
                              + Engine.formatDuration(row.groupDuration)
                        color: Theme.dim
                        font.pixelSize: Theme.smallFontSize + 1
                        elide: Text.ElideRight
                    }
                }

                // Track row
                Item {
                    visible: !row.header
                    x: pane.gutter
                    width: pane.numberWidth
                    height: parent.height
                    Text {
                        anchors.fill: parent
                        visible: !row.isPlaying
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: row.number > 0 ? row.number : ""
                        color: Theme.faint
                        font.pixelSize: Theme.fontSize
                        font.features: { "tnum": 1 }
                    }
                    Icon {
                        visible: row.isPlaying
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        height: 12
                        name: Engine.current.player.playing ? "play" : "pause"
                        color: Theme.accent
                    }
                }
                Text {
                    visible: !row.header
                    x: pane.gutter + pane.numberWidth + 10
                    width: parent.width - x - pane.lengthWidth - 24
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: row.title
                    color: row.isPlaying || row.isSelected ? Qt.lighter(Theme.accent, 1.25) : Theme.text
                    font.pixelSize: Theme.fontSize
                    elide: Text.ElideRight
                }
                Text {
                    visible: !row.header
                    x: parent.width - pane.lengthWidth - 12
                    width: pane.lengthWidth
                    height: parent.height
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                    text: row.duration > 0 ? Engine.formatDuration(row.duration) : ""
                    color: row.isPlaying || row.isSelected ? Qt.lighter(Theme.accent, 1.25) : Theme.dim
                    font.pixelSize: Theme.fontSize
                    font.features: { "tnum": 1 }
                }
            }

            Column {
                anchors.centerIn: parent
                visible: view.count === 0 && !Engine.tracks.loading && Engine.current.connected
                spacing: 6
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Engine.tracks.listId === "" ? "No list open" : "This list is empty"
                    color: Theme.text
                    font.pixelSize: 15
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Add albums from the library to Up Next with the queue button."
                    color: Theme.dim
                    font.pixelSize: Theme.fontSize
                }
            }
        }
    }
}
