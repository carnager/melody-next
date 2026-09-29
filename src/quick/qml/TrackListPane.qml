// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: pane

    property var dragGhost
    readonly property var tracks: Engine.tracks
    property var selected: ({})
    property int selectionRevision: 0
    property int anchorRow: -1

    readonly property int gutter: 66
    readonly property int numberWidth: 26
    readonly property int lengthWidth: 56
    readonly property int ratingWidth: 76

    // Tabs are the lists this window has open, of any engine, working or
    // saved. Another window's working lists are its tabs, not this one's:
    // they are one choice away under +. Lists are named "<engine>|<list>".
    Settings {
        id: remembered
        // Its own file: the widgets window's settings are not this one's to write.
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/trackknife/quick-window.conf"
        category: "QuickWindow"
        property string currentList: ""
        property var open: []
        property bool followPlayback: false
        // Engines met before: one whose tabs were all closed stays so.
        property var engines: []
    }

    readonly property var engineColors: ["#6cc28b", Theme.accent, "#d6a04c", "#c07bd6", "#5fb8c9"]

    function tabKey(session, listId) {
        return session.key + "|" + listId;
    }

    function isTab(session, listId) {
        return remembered.open.indexOf(tabKey(session, listId)) >= 0;
    }

    function show(session, listId) {
        remembered.currentList = tabKey(session, listId);
        Engine.show(session, listId);
    }

    function open(session, listId) {
        const key = tabKey(session, listId);
        if (remembered.open.indexOf(key) < 0)
            remembered.open = remembered.open.concat([key]);
        show(session, listId);
    }

    function close(session, listId) {
        const key = tabKey(session, listId);
        const at = remembered.open.indexOf(key);
        remembered.open = remembered.open.filter(k => k !== key);
        if (Engine.tracks.session !== session || Engine.tracks.listId !== listId)
            return;
        // The tab beside it, as a browser does; nothing when it was the last.
        const next = remembered.open[Math.min(at, remembered.open.length - 1)];
        if (next === undefined) {
            remembered.currentList = "";
            Engine.show(null, "");
            return;
        }
        const separator = next.indexOf("|");
        show(Engine.sessionByKey(next.slice(0, separator)), next.slice(separator + 1));
    }

    // When an engine's lists arrive: tabs of lists that are gone close; an
    // engine met for the first time opens the list it changed last; and
    // the list on show is put back.
    function adoptLists(session) {
        const prefix = session.key + "|";
        remembered.open = remembered.open.filter(
            k => !k.startsWith(prefix) || session.lists.indexOf(k.slice(prefix.length)) >= 0);
        if (remembered.engines.indexOf(session.key) < 0) {
            remembered.engines = remembered.engines.concat([session.key]);
            const newest = session.lists.newest();
            if (newest !== "")
                open(session, newest);
        }
        if (remembered.currentList.startsWith(prefix)) {
            const listId = remembered.currentList.slice(prefix.length);
            if (isTab(session, listId))
                Engine.show(session, listId);
        } else if (Engine.tracks.session === null && remembered.open.length > 0) {
            // Something to look at until the remembered list's engine
            // answers -- shown, not remembered, so it does not replace it.
            const first = remembered.open[0];
            if (first.startsWith(prefix))
                Engine.show(session, first.slice(prefix.length));
        }
    }

    Instantiator {
        model: Engine.sessions
        delegate: Item {
            required property var modelData
            Connections {
                target: modelData.lists
                function onLoaded() {
                    pane.adoptLists(modelData);
                }
            }
            Connections {
                target: modelData
                function onListCreated(listId) {
                    pane.open(modelData, listId);
                }
            }
        }
    }

    // The list shown again after an edit keeps its place.
    property real keptY: NaN
    Connections {
        target: Engine.tracks
        function onRefreshing() {
            pane.keptY = view.contentY;
        }
        function onLoaded() {
            pane.clearSelection();
            if (!isNaN(pane.keptY)) {
                view.contentY = pane.keptY;
                pane.keptY = NaN;
            }
        }
    }

    // Find in list (ADR-0125): selects matches, never filters or plays.
    property bool finding: false
    property bool findMissed: false

    function openFind() {
        finding = true;
        findField.forceActiveFocus();
        findField.selectAll();
    }

    function closeFind() {
        finding = false;
        findMissed = false;
        view.forceActiveFocus();
    }

    // From the selection; `again` moves past it, otherwise it may match.
    function findStep(step, again) {
        let from = anchorRow;
        if (from < 0)
            from = step > 0 ? -1 : view.count;
        else if (!again)
            from -= step;
        const row = Engine.tracks.find(findField.text, from, step);
        findMissed = row < 0 && findField.text.trim() !== "";
        if (row >= 0) {
            select(row, Qt.NoModifier);
            view.positionViewAtIndex(row, ListView.Center);
        }
    }

    function jumpToPlaying() {
        const row = Engine.tracks.rowOfEntry(Engine.current.player.entry);
        if (row < 0)
            return;
        select(row, Qt.NoModifier);
        view.positionViewAtIndex(row, ListView.Center);
    }

    // Keeps what plays in view as it changes.
    readonly property string playingEntry: Engine.current.player.entry
    onPlayingEntryChanged: {
        if (remembered.followPlayback)
            jumpToPlaying();
    }
    property alias followPlayback: remembered.followPlayback

    // The tab on show, for the shortcuts.
    function renameShown() {
        const session = Engine.tracks.session, id = Engine.tracks.listId;
        if (session === null || id === "")
            return;
        nameDialog.ask("Rename list", "New name:", session.lists.nameOf(id), name => session.renameList(id, name));
    }

    function saveShown() {
        const session = Engine.tracks.session, id = Engine.tracks.listId;
        if (session === null || id === "" || session.lists.isSaved(id))
            return;
        nameDialog.ask("Save as playlist", "Kept on " + session.name + " as:", session.lists.nameOf(id),
                       name => session.saveList(id, name));
    }

    function closeShown() {
        if (Engine.tracks.session !== null && Engine.tracks.listId !== "")
            close(Engine.tracks.session, Engine.tracks.listId);
    }

    function duplicateShown() {
        const session = Engine.tracks.session, id = Engine.tracks.listId;
        if (session !== null && id !== "")
            session.duplicateList(id, session.lists.nameOf(id) + " (copy)");
    }

    Connections {
        target: Engine.tracks
        function onListIdChanged() {
            // Another tab: what was being found was found in the last one.
            if (pane.finding)
                pane.closeFind();
        }
    }

    function removeSelected() {
        const rows = selectedRows();
        if (rows.length > 0)
            Engine.tracks.removeRows(rows);
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
            delegate: MenuItem {
                required property var modelData
                text: Engine.sessions.length > 1 ? "New list on " + modelData.name : "New list"
                enabled: modelData.connected
                onTriggered: pane.newList(modelData)
            }
            onObjectAdded: (index, object) => savedMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => savedMenu.removeItem(object)
        }
        MenuSeparator {}
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
                        text: name + "  ·  " + tracks + (tracks === 1 ? " track" : " tracks")
                        font.italic: !saved
                        checkable: true
                        checked: pane.isTab(engineMenu.modelData, listId)
                        onTriggered: pane.open(engineMenu.modelData, listId)
                    }
                    onObjectAdded: (index, object) => engineMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => engineMenu.removeItem(object)
                }
            }
            onObjectAdded: (index, object) => savedMenu.insertMenu(index + Engine.sessions.length + 1, object)
            onObjectRemoved: (index, object) => savedMenu.removeMenu(object)
        }
    }

    NameDialog { id: nameDialog }
    ConfirmDialog { id: confirmDialog }

    function newList(session) {
        nameDialog.ask("New list", "A working list on " + session.name + ":", "New list",
                       name => session.createList(name));
    }

    // What the tab menu acts on.
    property var menuSession: null
    property string menuList: ""
    property string menuName: ""
    property bool menuSaved: false

    Menu {
        id: tabMenu
        MenuItem {
            text: "Rename…"
            onTriggered: {
                const session = pane.menuSession, id = pane.menuList;
                nameDialog.ask("Rename list", "New name:", pane.menuName, name => session.renameList(id, name));
            }
        }
        MenuItem {
            text: "Save as playlist…"
            visible: !pane.menuSaved
            height: visible ? implicitHeight : 0
            onTriggered: {
                const session = pane.menuSession, id = pane.menuList;
                nameDialog.ask("Save as playlist", "Kept on " + session.name + " as:", pane.menuName,
                               name => session.saveList(id, name));
            }
        }
        MenuSeparator {}
        MenuItem {
            text: "Close tab"
            onTriggered: pane.close(pane.menuSession, pane.menuList)
        }
        MenuItem {
            text: "Delete…"
            onTriggered: {
                const session = pane.menuSession, id = pane.menuList;
                confirmDialog.ask("Delete list",
                                  "Delete “" + pane.menuName + "” from " + session.name
                                  + "? Every window loses it, and it cannot be undone.",
                                  () => session.deleteList(id));
            }
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
        MenuSeparator {}
        MenuItem {
            text: "Remove from list"
            onTriggered: pane.removeSelected()
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

                            Repeater {
                                model: group.session.lists
                                delegate: Rectangle {
                                    id: tab
                                    required property string listId
                                    required property string name
                                    required property bool saved
                                    readonly property bool current: Engine.tracks.session === group.session
                                                                    && Engine.tracks.listId === listId

                                    visible: pane.isTab(group.session, listId)
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
                                    // Dropped on a tab: added at the end of that list.
                                    DropArea {
                                        id: tabDrop
                                        anchors.fill: parent
                                        keys: ["trackknife"]
                                        onEntered: drag => {
                                            const payload = drag.source.payload;
                                            drag.accepted = payload !== null && payload.session === group.session
                                                && (payload.kind === "library"
                                                    || (payload.kind === "tracks" && payload.listId !== tab.listId));
                                        }
                                        onDropped: drop => {
                                            const payload = drop.source.payload;
                                            if (payload.kind === "library")
                                                payload.session.library.addToList(payload.row, tab.listId, -1);
                                            else
                                                Engine.tracks.copyToList(payload.rows, tab.listId);
                                            drop.accept();
                                        }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: parent.radius
                                        color: "transparent"
                                        border.color: Theme.accent
                                        border.width: 2
                                        visible: tabDrop.containsDrag
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                                        onClicked: mouse => {
                                            if (mouse.button === Qt.MiddleButton) {
                                                pane.close(group.session, tab.listId);
                                            } else if (mouse.button === Qt.RightButton) {
                                                pane.menuSession = group.session;
                                                pane.menuList = tab.listId;
                                                pane.menuName = tab.name;
                                                pane.menuSaved = tab.saved;
                                                tabMenu.popup();
                                            } else {
                                                pane.show(group.session, tab.listId);
                                            }
                                        }
                                        onDoubleClicked: mouse => {
                                            if (mouse.button !== Qt.LeftButton)
                                                return;
                                            const session = group.session, id = tab.listId;
                                            nameDialog.ask("Rename list", "New name:", tab.name,
                                                           name => session.renameList(id, name));
                                        }
                                    }
                                    // Whose list, when there is more than one engine.
                                    Rectangle {
                                        id: engineDot
                                        x: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Engine.sessions.length > 1 ? 7 : 0
                                        height: 7
                                        radius: 3.5
                                        color: pane.engineColors[group.session.index % pane.engineColors.length]
                                        opacity: group.session.connected ? 1 : 0.35
                                    }
                                    Text {
                                        id: tabLabel
                                        anchors.left: engineDot.right
                                        anchors.leftMargin: Engine.sessions.length > 1 ? 7 : 2
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
                                        anchors.right: parent.right
                                        anchors.rightMargin: 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        icon: "close"
                                        iconSize: 11
                                        tip: "Close"
                                        opacity: tab.current || tabHover.hovered ? 1 : 0
                                        onClicked: pane.close(group.session, tab.listId)
                                    }

                                    ToolTip.visible: tabHover.hovered && Engine.sessions.length > 1
                                    ToolTip.delay: 600
                                    ToolTip.text: group.session.name + (tab.saved ? "" : " · working list")
                                }
                            }
                        }
                    }

                    IconButton {
                        id: openButton
                        anchors.verticalCenter: parent.verticalCenter
                        icon: "plus"
                        iconSize: 13
                        tip: "New or open a list"
                        onClicked: savedMenu.popup(openButton, 0, openButton.height)
                    }
                }
            }
        }

        // Find in list
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: pane.finding ? 38 : 0
            visible: implicitHeight > 0
            clip: true
            color: Theme.panel
            Behavior on implicitHeight { NumberAnimation { duration: 120 } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4
                TextField {
                    id: findField
                    Layout.preferredWidth: 320
                    placeholderText: "Find in list"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    font.pixelSize: Theme.fontSize
                    onTextChanged: pane.findStep(1, false)
                    Keys.onReturnPressed: event => pane.findStep(event.modifiers & Qt.ShiftModifier ? -1 : 1, true)
                    Keys.onEnterPressed: event => pane.findStep(event.modifiers & Qt.ShiftModifier ? -1 : 1, true)
                    Keys.onEscapePressed: pane.closeFind()
                    background: Rectangle {
                        radius: 5
                        color: Theme.window
                        border.color: pane.findMissed ? Theme.error : findField.activeFocus ? Theme.accent : Theme.line
                    }
                }
                IconButton {
                    icon: "chevron"
                    rotation: -90
                    iconSize: 12
                    tip: "Previous (Shift+Enter)"
                    onClicked: pane.findStep(-1, true)
                }
                IconButton {
                    icon: "chevron"
                    rotation: 90
                    iconSize: 12
                    tip: "Next (Enter)"
                    onClicked: pane.findStep(1, true)
                }
                Text {
                    visible: pane.findMissed
                    text: "Not in this list"
                    color: Theme.error
                    font.pixelSize: Theme.smallFontSize
                }
                Item { Layout.fillWidth: true }
                IconButton {
                    icon: "close"
                    iconSize: 11
                    tip: "Close (Escape)"
                    onClicked: pane.closeFind()
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
                x: parent.width - pane.lengthWidth - pane.ratingWidth - 12
                anchors.verticalCenter: parent.verticalCenter
                text: "Rating"
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

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: view
                anchors.fill: parent
                clip: true
                focus: true
                model: Engine.tracks
                reuseItems: true
                cacheBuffer: 400
                boundsBehavior: Flickable.StopAtBounds
                // A mouse drag drags rows; the wheel and touch still scroll.
                acceptedButtons: Qt.NoButton
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
                    } else if (event.key === Qt.Key_Delete) {
                        pane.removeSelected();
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
                    required property int rating

                    readonly property bool isSelected: pane.selectionRevision >= 0 && !!pane.selected[index]
                    readonly property bool isPlaying: !header && entry !== "" && entry === Engine.current.player.entry

                    width: ListView.view.width
                    height: header ? 30 : Theme.rowHeight
                    // The album cover hangs into the rows below its header.
                    z: header ? 2 : 1

                    HoverHandler { id: rowHover }
                    DragHandler {
                        target: null
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                        onActiveChanged: {
                            if (!active) {
                                pane.dragGhost.end();
                                return;
                            }
                            if (!row.isSelected) {
                                if (row.header)
                                    pane.selectAlbum(row.index);
                                else
                                    pane.select(row.index, Qt.NoModifier);
                            }
                            const rows = pane.selectedRows();
                            pane.dragGhost.begin({ kind: "tracks", session: Engine.tracks.session,
                                                   listId: Engine.tracks.listId, rows: rows },
                                                 rows.length === 1 ? row.title : rows.length + " tracks",
                                                 centroid.scenePosition);
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
                        width: parent.width - x - pane.lengthWidth - pane.ratingWidth - 24
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
                        width: parent.width - x - pane.lengthWidth - pane.ratingWidth - 24
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        text: row.title
                        color: row.isPlaying || row.isSelected ? Theme.accentText : Theme.text
                        font.pixelSize: Theme.fontSize
                        elide: Text.ElideRight
                    }
                    // A track's rating, or on a header its album's. Unrated
                    // shows only under the pointer; unknown not at all.
                    Stars {
                        visible: row.rating >= 0 && (row.rating > 0 || rowHover.hovered)
                        x: parent.width - pane.lengthWidth - pane.ratingWidth - 12
                        y: row.header ? parent.height - height - 3 : (parent.height - height) / 2
                        rating: Math.max(0, row.rating)
                        onRated: value => Engine.tracks.rate(row.index, value)
                    }
                    Text {
                        visible: !row.header
                        x: parent.width - pane.lengthWidth - 12
                        width: pane.lengthWidth
                        height: parent.height
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: row.duration > 0 ? Engine.formatDuration(row.duration) : ""
                        color: row.isPlaying || row.isSelected ? Theme.accentText : Theme.dim
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

            // Dropped tracks go where the line shows: library rows are added
            // there, the list's own rows move there.
            DropArea {
                id: listDrop
                anchors.fill: parent
                keys: ["trackknife"]

                // The view row the drop goes before; count for the end.
                property int insertRow: -1

                function accepts(payload) {
                    return payload !== null && Engine.tracks.session !== null
                        && payload.session === Engine.tracks.session && Engine.tracks.listId !== ""
                        && (payload.kind === "library"
                            || (payload.kind === "tracks" && payload.listId === Engine.tracks.listId));
                }

                function place(y) {
                    const contentY = y + view.contentY;
                    const row = view.indexAt(view.width / 2, contentY);
                    if (row < 0) {
                        insertRow = contentY < 0 ? 0 : view.count;
                        return;
                    }
                    const item = view.itemAtIndex(row);
                    // A header takes the drop before its album's first track.
                    if (item === null || Engine.tracks.groupRows(row).length > 0)
                        insertRow = row;
                    else
                        insertRow = contentY - item.y > item.height / 2 ? row + 1 : row;
                }

                onEntered: drag => {
                    drag.accepted = accepts(drag.source.payload);
                    if (drag.accepted)
                        place(drag.y);
                }
                onPositionChanged: drag => place(drag.y)
                onExited: insertRow = -1
                onDropped: drop => {
                    const payload = drop.source.payload;
                    const before = Engine.tracks.itemIndexAt(insertRow);
                    if (payload.kind === "tracks")
                        Engine.tracks.moveRows(payload.rows, before);
                    else if (payload.kind === "library")
                        payload.session.library.addToList(payload.row, Engine.tracks.listId, before);
                    insertRow = -1;
                    drop.accept();
                }

                // Scrolls while a drag rests near an edge.
                Timer {
                    interval: 30
                    repeat: true
                    running: listDrop.containsDrag
                    onTriggered: {
                        const y = listDrop.drag.y;
                        if (y < 30)
                            view.contentY = Math.max(view.originY - view.topMargin, view.contentY - 12);
                        else if (y > listDrop.height - 30)
                            view.contentY = Math.min(view.contentHeight + view.originY + view.bottomMargin - view.height,
                                                     view.contentY + 12);
                        listDrop.place(y);
                    }
                }
            }

            Rectangle {
                visible: listDrop.containsDrag && listDrop.insertRow >= 0
                x: pane.gutter - 6
                width: parent.width - x - 6
                height: 2
                radius: 1
                color: Theme.accent
                y: {
                    const row = listDrop.insertRow;
                    const item = view.itemAtIndex(Math.min(row, view.count - 1));
                    if (item === null)
                        return view.topMargin;
                    const top = row >= view.count ? item.y + item.height : item.y;
                    return top - view.contentY - 1;
                }
            }
        }
    }
}
