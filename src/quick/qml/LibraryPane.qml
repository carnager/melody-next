// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// One engine's library (LocalLibraryPanel): a search row with the query
// toggle, recently added, refresh and the folders; the tree of artists,
// albums and tracks, with append / insert next / replace on the row under
// the pointer; a quiet line of news at the foot.
Item {
    id: pane

    signal foldersRequested()

    readonly property var browser: Tk.library
    readonly property color ground: palette.window
    readonly property color ink: palette.text
    property int coverRevision: 0

    Connections {
        target: pane.browser
        ignoreUnknownSignals: true
        // A row asked open before its children are there waits for them,
        // as the widgets tree's pending expansions do.
        function onExpandRequested(index) {
            pane.pendingExpansions.push(index);
            pane.completeExpansions();
        }
        function onReloadStarted() {
            pane.pendingExpansions = [];
        }
        function onLevelLoaded() {
            pane.completeExpansions();
        }
        function onCurrentRequested(index, focus) {
            tree.expandToIndex(index);
            tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                | ItemSelectionModel.Rows);
            const row = tree.rowAtIndex(index);
            if (row >= 0)
                tree.positionViewAtRow(row, TableView.Contain);
            if (focus)
                tree.forceActiveFocus();
        }
        function onCoverLoaded() {
            pane.coverRevision++;
        }
    }
    Connections {
        target: Tk
        function onLibrarySearchFocused() {
            search.forceActiveFocus();
            search.selectAll();
        }
    }

    // QA hook: a row opened as a click on its arrow opens it.
    function toggleRowForScreenshot(row) {
        tree.toggle(row, true);
    }

    property var pendingExpansions: []
    function completeExpansions() {
        const waiting = [];
        for (const index of pendingExpansions) {
            if (!index.valid)
                continue;
            if (!tree.model.hasChildren(index)) {
                waiting.push(index);
                continue;
            }
            tree.expandToIndex(index);
            const row = tree.rowAtIndex(index);
            if (row >= 0)
                tree.expand(row);
        }
        pendingExpansions = waiting;
    }

    function selectedIndexes() {
        return tree.selectionModel.selectedIndexes.filter(index => index.column === 0);
    }
    function requestAt(row, action) {
        const index = tree.index(row, 0);
        if (!tree.selectionModel.isSelected(index))
            tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                | ItemSelectionModel.Rows);
        pane.browser.request(pane.selectedIndexes(), action);
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            TextField {
                id: search
                objectName: "local-library-search"
                Layout.fillWidth: true
                placeholderText: pane.browser && pane.browser.queryMode
                                 ? qsTr("tkq query, e.g. genre HAS jazz")
                                 : qsTr("Search albums and tracks")
                Accessible.name: qsTr("Search local library")
                text: pane.browser ? pane.browser.search : ""
                onTextEdited: pane.browser.search = text
                onAccepted: pane.browser.commitSearch()
            }
            // ADR-0150: legible at a glance, so a checkbox.
            CheckBox {
                objectName: "local-library-query-toggle"
                text: qsTr("Query")
                checked: pane.browser ? pane.browser.queryMode : false
                onToggled: pane.browser.queryMode = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Interpret the search as a tkq query, e.g. genre HAS jazz AND date GREATER 1990")
            }
        }
        Label {
            objectName: "local-library-query-error"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.WordWrap
            text: pane.browser ? pane.browser.queryError : ""
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: pane.palette.base

            TreeView {
                id: tree
                objectName: "local-library-tree"
                anchors.fill: parent
                clip: true
                model: pane.browser ? pane.browser.model : null
                boundsBehavior: Flickable.StopAtBounds
                selectionModel: ItemSelectionModel {}
                selectionBehavior: TableView.SelectRows
                selectionMode: TableView.ExtendedSelection
                ScrollBar.vertical: ScrollBar {}
                Accessible.name: qsTr("Local artists, albums, and tracks")
                property int hoveredRow: -1

                // Opened before its children are asked for: once asked for,
                // a row with none yet reads as having none, and would not
                // open; if it still does not, it opens when they arrive.
                // Rows an opening shows, fading and sliding in below it.
                property int revealFrom: -1
                property int revealDepth: 0
                property double revealAt: 0

                function toggle(row, open) {
                    const index = tree.index(row, 0);
                    if (open) {
                        if (Tk.panelAnimations) {
                            tree.revealFrom = row;
                            tree.revealDepth = tree.depth(row);
                            tree.revealAt = Date.now();
                        }
                        tree.expand(row);
                        if (!tree.isExpanded(row))
                            pane.pendingExpansions.push(index);
                        pane.browser.noteExpanded(index, true);
                    } else {
                        tree.collapse(row);
                        pane.browser.noteExpanded(index, false);
                    }
                }

                // The covers of the albums in view are asked for.
                onContentYChanged: coverTimer.restart()
                onHeightChanged: coverTimer.restart()
                Timer {
                    id: coverTimer
                    interval: 0
                    onTriggered: {
                        if (!pane.browser)
                            return;
                        const keys = [];
                        for (let row = Math.max(0, tree.topRow); row <= tree.bottomRow; ++row) {
                            const index = tree.index(row, 0);
                            if (tree.model.data(index, 256 + 23) === "album"
                                    && tree.model.data(index, 256 + 26))
                                keys.push(tree.model.data(index, 256 + 21));
                        }
                        pane.browser.wantCovers(keys);
                    }
                }
                Connections {
                    target: tree.model
                    ignoreUnknownSignals: true
                    function onRowsInserted() {
                        coverTimer.restart();
                    }
                }

                delegate: Item {
                    id: node
                    required property TreeView treeView
                    required property bool isTreeNode
                    required property bool expanded
                    required property bool hasChildren
                    required property int depth
                    required property int row
                    required property bool current
                    required property bool selected
                    required property string display
                    required property string kind
                    required property string secondary
                    required property string count
                    required property var rating
                    required property bool available
                    required property var coverKey
                    required property var tooltip

                    readonly property bool root: depth === 0
                    readonly property bool track: kind === "track"
                    readonly property bool album: kind === "album"
                    readonly property bool artist: kind === "artist"
                    readonly property int iconExtent: track ? 16 : album ? 30 : 22
                    readonly property bool actions: available && kind !== ""
                                                    && (tree.hoveredRow === row
                                                        || (tree.activeFocus && current))


                    // Shown by an opening just now: in with a fade and a slide.
                    function reveal() {
                        const recent = Date.now() - tree.revealAt < 400;
                        const below = tree.revealFrom >= 0 && node.row > tree.revealFrom;
                        if (recent && below && node.depth > tree.revealDepth)
                            revealing.restart();
                    }
                    Component.onCompleted: reveal()
                    TableView.onReused: reveal()
                    transform: Translate {
                        id: shift
                    }
                    ParallelAnimation {
                        id: revealing
                        NumberAnimation {
                            target: node
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: Theme.moderate
                        }
                        NumberAnimation {
                            target: shift
                            property: "y"
                            from: -6
                            to: 0
                            duration: Theme.moderate
                            easing.type: Easing.OutCubic
                        }
                    }

                    implicitWidth: tree.width
                    implicitHeight: track ? 26 : album ? 40 : artist ? 30 : root ? 34 : 30

                    HoverHandler {
                        onHoveredChanged: {
                            if (hovered)
                                tree.hoveredRow = node.row;
                            else if (tree.hoveredRow === node.row)
                                tree.hoveredRow = -1;
                        }
                    }
                    ToolTip.visible: hover.hovered && (node.tooltip ?? "") !== ""
                    ToolTip.delay: Qt.styleHints.mousePressAndHoldInterval
                    ToolTip.text: node.tooltip ?? ""
                    HoverHandler {
                        id: hover
                    }

                    // Selection as a tint, as in the lists.
                    Rectangle {
                        anchors.fill: parent
                        visible: node.selected
                        color: Shade.mix(node.palette.base, node.palette.highlight, 0.32)
                    }
                    Label {
                        id: arrow
                        x: 2 + node.depth * 16
                        width: 12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: node.hasChildren
                        text: "▸"
                        horizontalAlignment: Text.AlignHCenter
                        rotation: node.expanded ? 90 : 0
                        Behavior on rotation {
                            enabled: Tk.panelAnimations
                            NumberAnimation {
                                duration: Theme.moderate
                                easing.type: Easing.OutCubic
                            }
                        }
                        color: node.palette.placeholderText
                        TapHandler {
                            onTapped: tree.toggle(node.row, !node.expanded)
                        }
                    }
                    // An artist has no picture: their initials on a tile
                    // coloured by their name.
                    InitialsTile {
                        id: tile
                        visible: node.artist
                        x: arrow.x + 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: node.iconExtent
                        height: node.iconExtent
                        name: node.display
                    }
                    Item {
                        id: picture
                        visible: !node.artist && node.kind !== ""
                        x: arrow.x + 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: node.iconExtent
                        height: node.iconExtent
                        Image {
                            id: cover
                            anchors.fill: parent
                            visible: node.album && status === Image.Ready && implicitWidth > 1
                            asynchronous: false
                            fillMode: Image.PreserveAspectFit
                            sourceSize: Qt.size(60, 60)
                            source: node.album && pane.browser
                                    ? "image://cover/" + encodeURIComponent("library/"
                                        + pane.browser.engineText() + "/" + node.coverKey)
                                      + "#" + pane.coverRevision
                                    : ""
                        }
                        // An album without a cover: its tile, as in the list.
                        InitialsTile {
                            anchors.fill: parent
                            visible: node.album && !cover.visible
                            opacity: node.available ? 1 : 0.45
                            name: node.display
                        }
                        Image {
                            anchors.fill: parent
                            visible: !node.album
                            sourceSize: Qt.size(node.iconExtent, node.iconExtent)
                            source: "image://icon/audio-x-generic"
                                    + (node.available ? "" : "?disabled")
                        }
                    }
                    Column {
                        x: (node.kind !== "" ? picture.x + node.iconExtent + (node.track ? 6 : 8)
                                             : arrow.x + 16)
                        width: (node.actions ? actionRow.x - 5 : countLabel.visible
                                ? countLabel.x - 8 : node.width - 8) - x
                        anchors.verticalCenter: parent.verticalCenter
                        Label {
                            width: parent.width
                            text: node.display
                            elide: Text.ElideRight
                            color: node.kind === "" && !node.root ? node.palette.placeholderText
                                                                  : node.palette.text
                            font.bold: node.root && node.kind === ""
                        }
                        Label {
                            width: parent.width
                            visible: node.secondary !== ""
                            text: node.secondary
                            elide: Text.ElideRight
                            color: Shade.alpha(node.palette.placeholderText, node.selected ? 1 : 220 / 255)
                            font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                        }
                    }
                    Label {
                        id: countLabel
                        visible: node.count !== "" && !node.actions
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: node.count
                        color: Shade.alpha(node.palette.placeholderText, 220 / 255)
                        font.pointSize: Math.max(7, Qt.application.font.pointSize - 1)
                    }
                    // Append, insert next, replace and play.
                    Row {
                        id: actionRow
                        // Above the row's own handling: its clicks are its own.
                        z: 3
                        visible: node.actions
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        Repeater {
                            model: [
                                {icon: "list-add|sp:SP_DialogOpenButton", label: qsTr("Append to current list")},
                                {icon: "go-next|sp:SP_ArrowRight", label: qsTr("Insert next in current list")},
                                {icon: "media-playback-start|sp:SP_MediaPlay", label: qsTr("Replace list and play")},
                            ]
                            // Shown and pointed at here; its click is taken
                            // by the row's one TapHandler below, so the row
                            // and its buttons never compete for a press.
                            delegate: Item {
                                id: action
                                required property var modelData
                                required property int index
                                readonly property bool isAction: true
                                width: 24
                                height: 24
                                Accessible.role: Accessible.Button
                                Accessible.name: modelData.label
                                HoverHandler {
                                    id: actionHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                ToolTip.visible: actionHover.hovered
                                ToolTip.delay: 600
                                ToolTip.text: modelData.label
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    radius: 4
                                    opacity: actionHover.hovered ? 1 : 0
                                    color: Shade.alpha(node.palette.highlight, node.selected ? 90 / 255 : 42 / 255)
                                    Behavior on opacity {
                                        NumberAnimation { duration: Theme.quick }
                                    }
                                }
                                Image {
                                    anchors.centerIn: parent
                                    sourceSize: Qt.size(12, 12)
                                    source: "image://icon/" + action.modelData.icon
                                }
                            }
                        }
                    }

                    // As the widgets tree: a click opens or closes a row with
                    // children anywhere on it; a track is taken on a click
                    // where the desktop activates on one, else by Enter or
                    // the row's own buttons. Double clicks do nothing more.
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: (eventPoint, button) => {
                            // A click on one of the row's buttons is the button's:
                            // its action, the row neither opened nor closed.
                            if (actionRow.visible) {
                                const at = actionRow.mapFromItem(node, eventPoint.position);
                                const hit = actionRow.childAt(at.x, at.y);
                                if (hit && hit.isAction) {
                                    pane.requestAt(node.row, hit.index);
                                    return;
                                }
                                if (actionRow.contains(at))
                                    return;
                            }
                            tree.forceActiveFocus();
                            const index = tree.index(node.row, 0);
                            const modifiers = eventPoint.modifiers;
                            const command = modifiers & Qt.ControlModifier
                                ? ItemSelectionModel.Toggle | ItemSelectionModel.Rows
                                : ItemSelectionModel.ClearAndSelect | ItemSelectionModel.Rows;
                            tree.selectionModel.setCurrentIndex(index, command);
                            pane.browser.noteCurrent(index);
                            if (modifiers & (Qt.ControlModifier | Qt.ShiftModifier))
                                return;
                            if (node.hasChildren)
                                tree.toggle(node.row, !node.expanded);
                            else if (tree.model.data(index, 256 + 4) || node.kind === "")
                                pane.browser.activate(index);
                            else if (Qt.styleHints.singleClickActivation)
                                pane.requestAt(node.row, 0);
                        }
                    }
                    // Dragged, the albums, artists or tracks chosen go where
                    // they are dropped: a list, a new one, Up Next.
                    DragSource {
                        enabled: node.kind !== "" && node.available
                        copyOnly: true
                        label: {
                            const count = tree.selectionModel.selectedIndexes.length;
                            return count > 1 ? qsTr("%1 items").arg(count) : node.display;
                        }
                        onBegan: {
                            const index = tree.index(node.row, 0);
                            if (!tree.selectionModel.isSelected(index))
                                tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                                    | ItemSelectionModel.Rows);
                            Tk.dragLibrary(pane.selectedIndexes());
                        }
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: {
                            if (node.kind === "")
                                return;
                            const index = tree.index(node.row, 0);
                            if (!tree.selectionModel.isSelected(index))
                                tree.selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect
                                                                    | ItemSelectionModel.Rows);
                            libraryMenu.open(node.row, index, node.kind, node.rating,
                                             node.hasChildren, node.expanded);
                        }
                    }
                }

                // Enter opens or closes a row with children, and takes the
                // rest.
                Keys.onReturnPressed: {
                    const index = tree.selectionModel.currentIndex;
                    if (!index.valid)
                        return;
                    const row = tree.rowAtIndex(index);
                    if (tree.model.hasChildren(index) && row >= 0)
                        tree.toggle(row, !tree.isExpanded(row));
                    else
                        pane.browser.request(pane.selectedIndexes(), 0);
                }
            }
        }

        // The footer: one small, quiet line of news, under a hairline.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Shade.mix(pane.ground, pane.ink, 0.12)
        }
        // Its news on the left; recently added, refresh and the library's
        // folders small on the right.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 2
            Layout.topMargin: 2
            spacing: 0
            Label {
                objectName: "local-library-status"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: pane.browser ? pane.browser.status : ""
                color: pane.palette.placeholderText
                font.pointSize: Qt.application.font.pointSize * 0.9
            }
            component StatusButton: ToolButton {
                flat: true
                display: AbstractButton.IconOnly
                icon.width: 16
                icon.height: 16
                // All four sides: the style's wider sides would leave the
                // icon a few pixels wide.
                padding: 4
                leftPadding: 4
                rightPadding: 4
                implicitWidth: 24
                implicitHeight: 24
                Accessible.name: text
                ToolTip.delay: 600
            }
            StatusButton {
                objectName: "local-library-newest"
                text: qsTr("Recently added")
                icon.source: "image://icon/document-open-recent|sp:SP_FileDialogDetailedView"
                checkable: true
                checked: pane.browser ? pane.browser.newestFirst : false
                onToggled: pane.browser.newestFirst = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Show albums newest first, as they came into the library")
            }
            StatusButton {
                objectName: "local-library-scan"
                readonly property bool scanning: pane.browser ? pane.browser.scanning : false
                text: scanning ? qsTr("Stop") : qsTr("Refresh")
                icon.source: scanning ? "image://icon/process-stop|sp:SP_BrowserStop" : "image://icon/view-refresh|sp:SP_BrowserReload"
                ToolTip.visible: hovered
                ToolTip.text: scanning ? qsTr("Stop scanning") : qsTr("Refresh")
                onClicked: pane.browser.toggleScan()
            }
            StatusButton {
                objectName: "local-library-folders"
                text: qsTr("Folders…")
                icon.source: "image://icon/folder|sp:SP_DirIcon"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Choose which folders belong to your music library")
                onClicked: foldersDialog.open()
            }
        }
        // Which library this is, when it does not answer (ADR-0220).
        Label {
            objectName: "local-library-source"
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            visible: pane.browser ? pane.browser.sourceShown : false
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: pane.browser ? pane.browser.source : ""
            font.pointSize: Qt.application.font.pointSize * 0.9
            ToolTip.visible: sourceHover.hovered
            ToolTip.text: pane.browser ? pane.browser.sourceTooltip : ""
            HoverHandler {
                id: sourceHover
            }
        }
    }

    Menu {
        id: libraryMenu
        objectName: "local-library-context-menu"
        property int row: -1
        property var index
        property string kind
        property int rating: 0
        property bool expandable: false
        property bool expanded: false
        property var targets: []
        readonly property bool available: true

        function open(row, index, kind, rating, expandable, expanded) {
            libraryMenu.row = row;
            libraryMenu.index = index;
            libraryMenu.kind = kind;
            libraryMenu.rating = rating ?? 0;
            libraryMenu.expandable = expandable;
            libraryMenu.expanded = expanded;
            libraryMenu.targets = Tk.libraryListTargets();
            popup();
        }

        MenuItem {
            objectName: "action-local-library-0"
            text: qsTr("Append to current list")
            icon.source: "image://icon/list-add"
            onTriggered: pane.browser.request(pane.selectedIndexes(), 0)
        }
        Menu {
            objectName: "local-library-add-to-list"
            title: qsTr("Add to list")
            enabled: libraryMenu.targets.length > 0
            Repeater {
                model: libraryMenu.targets
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: pane.browser.addToList(pane.selectedIndexes(), modelData.id)
                }
            }
        }
        Repeater {
            model: [
                {action: 1, label: qsTr("Insert next in current list"), icon: "go-next"},
                {action: 2, label: qsTr("Replace list and play"), icon: "media-playback-start"},
                {action: 3, label: qsTr("Open in new tab"), icon: "tab-new"},
                {action: 4, label: qsTr("Play next (Up Next)"), icon: "media-playlist-append"},
                {action: 5, label: qsTr("Add to Up Next"), icon: "media-playlist-append"},
            ]
            delegate: MenuItem {
                required property var modelData
                objectName: "action-local-library-" + modelData.action
                text: modelData.label
                icon.source: "image://icon/" + modelData.icon
                onTriggered: pane.browser.request(pane.selectedIndexes(), modelData.action)
            }
        }
        MenuSeparator {
            visible: libraryMenu.kind !== "artist"
        }
        Menu {
            objectName: "local-library-rate-menu"
            title: libraryMenu.kind === "album" ? qsTr("Rate album") : qsTr("Rate track")
            enabled: libraryMenu.kind !== "artist"
            Repeater {
                model: [0, 2, 4, 6, 8, 10]
                delegate: RatingMenuItem {
                    required property int modelData
                    objectName: "action-local-library-rate-" + modelData
                    rating: modelData
                    shared: libraryMenu.rating === modelData
                    onTriggered: pane.browser.rate(libraryMenu.index, modelData)
                }
            }
        }
        MenuSeparator {
            visible: libraryMenu.expandable
        }
        MenuItem {
            visible: libraryMenu.expandable
            height: visible ? implicitHeight : 0
            text: libraryMenu.expanded ? qsTr("Collapse") : qsTr("Expand")
            onTriggered: tree.toggle(libraryMenu.row, !libraryMenu.expanded)
        }
    }

    LibraryFoldersDialog {
        id: foldersDialog
        browser: pane.browser
    }
}
