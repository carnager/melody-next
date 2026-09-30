// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Trackknife.Quick

// The Trackknife window (BenchMainWindow), in Qt Quick: the transport on
// top, the sources and the track lists side by side, Up Next on the right,
// the status bar below; the menus File, Edit, Workspace and Playback.
ApplicationWindow {
    id: window

    width: 1100
    height: 720
    visible: true
    title: Tk.transport.windowTitle ?? "Trackknife"

    Settings {
        id: upNextSettings
        category: "up-next"
        property bool visible: false
        property int width: 300
    }

    // What a menu or a shortcut has not been ported to yet says so, rather
    // than doing nothing.
    function notYet(what) {
        status.showMessage(what + " is not in the Qt Quick window yet", 3000);
    }
    // Settings: one window at a time, brought forward on the page asked for.
    property var settingsWindow: null
    readonly property var settingsPages: ({general: 0, playback: 1, library: 2, engine: 3,
                                           naming: 4, replaygain: 5, covers: 6,
                                           "metadata-services": 7, lastfm: 8, shortcuts: 9})
    function openSettings(page) {
        if (!settingsWindow) {
            const settings = Tk.openSettings();
            settings.loadShortcuts(window.shortcutCommands());
            settingsWindow = settingsComponent.createObject(window, {settings: settings});
        }
        settingsWindow.showPage(typeof page === "string" ? (settingsPages[page] ?? 0) : (page ?? 0));
        return settingsWindow;
    }
    // QA hook (--screenshot with --open): a menu or dialog opened by name,
    // the first row selected, for a picture of it.
    function openForScreenshot(name) {
        Tk.rows.press(0, 0);
        const popups = {track: trackMenu, header: headerMenu, tab: tabMenu};
        if (popups[name])
            popups[name].popup(window.width / 2, window.height / 3);
        else if (name === "editbar")
            editBar.openSort();
        else if (name === "folders")
            Tk.selectSource(0, false);
        else if (name === "artist")
            sources.openArtistForScreenshot();
        else if (name === "album") {
            sources.openArtistForScreenshot();
            Qt.callLater(() => albumTimer.start());
        }
        else if (name === "convert") {
            Tk.rows.selectAll();
            Tk.convertFiles();
        }
        else if (name === "replaygain") {
            Tk.rows.selectAll();
            window.replayGainForScreenshot = true;
            Tk.replayGain();
        }
        else if (name.startsWith("tagger")) {
            Tk.rows.selectAll();
            window.taggerForScreenshot = name;
            Tk.editTags();
        }
        else if (name === "search" && Tk.library)
            Tk.library.search = "a";
        else if (name === "dynamic") {
            window.openDynamic();
            window.dynamicWindow.dynamic.setQuery("ALL");
            window.dynamicWindow.dynamic.refresh();
        }
        else if (name === "searchwindow") {
            window.openSearch();
            window.searchWindow.search.setScope(1);
            window.searchWindow.search.setText("tone");
        }
        else if (name === "palette") {
            commandPaletteDialog.commands = window.paletteCommands();
            commandPaletteDialog.open();
        }
        else if (name === "quickalbum")
            window.openQuickPick(true);
        else if (name === "openlist")
            window.openListWindow = openListComponent.createObject(window, {openList: Tk.openList()});
        else if (name === "find") {
            Tk.openFind();
            Tk.setFindQuery("tone");
        }
        else if (name === "listspane")
            Tk.listsInPanel = true;
        else if (name === "libraryfolders")
            window.openSettings("library");
        else if (name.startsWith("settings"))
            window.openSettings(name.length > 9 ? name.substring(9) : "general");
    }
    property string taggerForScreenshot: ""
    property bool replayGainForScreenshot: false
    Timer {
        id: stageTimer
        property var opened
        interval: 1500
        onTriggered: opened.stageForScreenshot(window.taggerForScreenshot)
    }
    Timer {
        id: albumTimer
        interval: 500
        onTriggered: sources.openRowForScreenshot(1)
    }
    function closeTab(index) {
        const tab = Tk.tabAt(index);
        if (tab.dirty === true && tab.pinned !== true) {
            discardDialog.index = index;
            discardDialog.text = "Discard the unsaved contents of “%1”?".arg(tab.name);
            discardDialog.open();
            return;
        }
        Tk.closeTab(index);
    }

    menuBar: MenuBar {
        Menu {
            title: "&File"
            KeyedAction {
                objectName: "action-new-list"
                text: "New list…"
                defaultKey: "Ctrl+N"
                onTriggered: nameDialog.ask("New list", "", name => Tk.newList(name))
            }
            KeyedAction {
                objectName: "action-open-files"
                text: "Open files…"
                defaultKey: "Ctrl+O"
                onTriggered: filesDialog.open()
            }
            KeyedAction {
                objectName: "action-open-folder"
                text: "Open folder…"
                defaultKey: "Ctrl+Shift+O"
                onTriggered: folderDialog.open()
            }
            KeyedAction {
                objectName: "action-import-m3u8"
                text: qsTr("Import M3U8 playlist…")
                onTriggered: importDialog.open()
            }
            KeyedAction {
                objectName: "action-export-m3u8"
                text: qsTr("Export list as M3U8…")
                enabled: Tk.currentTab >= 0
                onTriggered: exportDialog.open()
            }
            KeyedAction {
                objectName: "action-open-list"
                text: "Open list…"
                defaultKey: "Ctrl+Alt+O"
                onTriggered: {
                    if (window.openListWindow)
                        window.openListWindow.requestActivate();
                    else
                        window.openListWindow = openListComponent.createObject(window, {openList: Tk.openList()});
                }
            }
            KeyedAction {
                objectName: "action-dynamic-playlists"
                text: "Dynamic playlists…"
                onTriggered: window.openDynamic()
            }
            KeyedAction {
                objectName: "action-backup-workspace"
                text: "Back up workspace database…"
                onTriggered: backupDialog.open()
            }
            KeyedAction {
                objectName: "action-restore-workspace"
                text: "Restore workspace database…"
                onTriggered: restoreDialog.open()
            }
            Action {
                text: "Bookmark folder…"
                onTriggered: bookmarkDialog.open()
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-close-window"
                text: "Close window"
                onTriggered: window.close()
            }
            KeyedAction {
                objectName: "action-quit"
                text: "Quit and stop playback"
                defaultKey: "Ctrl+Q"
                onTriggered: Tk.quitAndStopEngine()
            }
        }
        Menu {
            title: "&Edit"
            KeyedAction {
                objectName: "action-find-in-list"
                text: qsTr("Find in current list…")
                defaultKey: "Ctrl+F"
                enabled: Tk.find.available ?? false
                onTriggered: Tk.openFind()
            }
            KeyedAction {
                objectName: "action-find-next-in-list"
                text: qsTr("Find next in list")
                defaultKey: "F3"
                enabled: Tk.find.available ?? false
                onTriggered: Tk.findNext(false)
            }
            KeyedAction {
                objectName: "action-find-previous-in-list"
                text: qsTr("Find previous in list")
                defaultKey: "Shift+F3"
                enabled: Tk.find.available ?? false
                onTriggered: Tk.findNext(true)
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-undo-list-edit"
                text: Tk.history.undoText ?? "Undo list edit"
                defaultKey: "Ctrl+Z"
                enabled: Tk.history.canUndo ?? false
                onTriggered: Tk.undoListEdit()
            }
            KeyedAction {
                objectName: "action-redo-list-edit"
                text: Tk.history.redoText ?? "Redo list edit"
                defaultKey: "Ctrl+Shift+Z"
                enabled: Tk.history.canRedo ?? false
                onTriggered: Tk.redoListEdit()
            }
            MenuSeparator {}
            SortMenu {
                onCustomRequested: editBar.openSort()
            }
            KeyedAction {
                objectName: "action-reverse-list"
                text: qsTr("Reverse list")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.reverse()
            }
            KeyedAction {
                objectName: "action-shuffle-albums"
                text: qsTr("Shuffle albums")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.shuffleAlbums()
            }
            KeyedAction {
                objectName: "action-deduplicate-list"
                text: qsTr("Remove duplicate entries")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.removeDuplicates()
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-edit-tags"
                text: "Edit tags…"
                defaultKey: "Alt+Return"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: Tk.editTags()
            }
            KeyedAction {
                objectName: "action-replaygain"
                text: "ReplayGain…"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: Tk.replayGain()
            }
            KeyedAction {
                objectName: "action-convert"
                text: "Convert files…"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: Tk.convertFiles()
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-settings"
                text: "Settings…"
                defaultKey: "Ctrl+,"
                onTriggered: window.openSettings(0)
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-remove-selected"
                text: "Remove selected"
                defaultKey: "Delete"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: Tk.removeSelectedRows()
            }
        }
        Menu {
            title: "&Workspace"
            KeyedAction {
                objectName: "action-command-palette"
                text: qsTr("Commands…")
                defaultKey: "Ctrl+Shift+P"
                onTriggered: {
                    commandPaletteDialog.commands = window.paletteCommands();
                    commandPaletteDialog.open();
                }
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-jump-to-playing"
                text: qsTr("Jump to playing")
                defaultKey: "Ctrl+J"
                onTriggered: Tk.jumpToPlaying()
            }
            KeyedAction {
                objectName: "action-follow-playback"
                text: qsTr("Cursor follows playback")
                defaultKey: "Ctrl+Shift+J"
                checkable: true
                checked: Tk.followPlayback
                onTriggered: Tk.followPlayback = checked
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-search-dialog"
                text: "Search…"
                defaultKey: "Ctrl+Shift+F"
                onTriggered: window.openSearch()
            }
            KeyedAction {
                objectName: "action-quick-album"
                text: qsTr("Quick album…")
                defaultKey: "Ctrl+Shift+A"
                onTriggered: window.openQuickPick(true)
            }
            KeyedAction {
                objectName: "action-quick-track"
                text: qsTr("Quick track…")
                defaultKey: "Ctrl+Shift+T"
                onTriggered: window.openQuickPick(false)
            }
            KeyedAction {
                objectName: "action-lists-panel"
                text: qsTr("Lists in a side panel")
                checkable: true
                checked: Tk.listsInPanel
                onTriggered: Tk.listsInPanel = checked
            }
            MenuSeparator {}
            KeyedAction {
                id: duplicateAction
                objectName: "action-duplicate-tab"
                text: "Duplicate tab"
                defaultKey: "Ctrl+Shift+D"
                enabled: Tk.currentTab >= 0
                onTriggered: Tk.duplicateTab()
            }
            KeyedAction {
                id: pinAction
                objectName: "action-pin-tab"
                text: "Pin tab"
                defaultKey: "Ctrl+Alt+P"
                checkable: true
                checked: Tk.list.pinned ?? false
                enabled: Tk.currentTab >= 0
                onTriggered: Tk.togglePinned()
            }
            KeyedAction {
                id: saveAction
                objectName: "action-save-list"
                text: "Save list"
                defaultKey: "Ctrl+S"
                enabled: Tk.currentTab >= 0
                onTriggered: {
                    if (Tk.list.scratch)
                        nameDialog.ask("Save working list", Tk.list.name, name => Tk.saveTab(name));
                    else
                        Tk.saveTab("");
                }
            }
            KeyedAction {
                id: renameAction
                objectName: "action-rename-tab"
                text: "Rename tab…"
                defaultKey: "F2"
                enabled: Tk.currentTab >= 0
                onTriggered: nameDialog.ask("Rename list", Tk.list.name, name => Tk.renameTab(name))
            }
            MenuSeparator {}
            KeyedAction {
                id: closeAction
                objectName: "action-close-tab"
                text: "Close tab"
                defaultKey: "Ctrl+W"
                enabled: Tk.currentTab >= 0
                onTriggered: window.closeTab(Tk.currentTab)
            }
            MenuSeparator {}
            TrackLayoutMenu {}
            MenuSeparator {}
            KeyedAction {
                objectName: "action-edit-panel-layout"
                text: "Edit panel layout"
                defaultKey: "Ctrl+Alt+L"
                checkable: true
                checked: Tk.panels.editing ?? false
                onTriggered: Tk.setPanelEditing(checked)
            }
            Menu {
                objectName: "menu-panel-arrangement"
                title: "Panel arrangement"
                enabled: Tk.panels.editing ?? false
                Action {
                    objectName: "action-layout-side-by-side"
                    text: "Side by side"
                    checkable: true
                    checked: Tk.panels.kind === "split" && !Tk.panels.vertical
                    onTriggered: Tk.arrangePanels("side")
                }
                Action {
                    objectName: "action-layout-top-bottom"
                    text: "Top and bottom"
                    checkable: true
                    checked: Tk.panels.kind === "split" && Tk.panels.vertical
                    onTriggered: Tk.arrangePanels("stacked")
                }
                Action {
                    objectName: "action-layout-tabbed"
                    text: "Tabbed stack"
                    checkable: true
                    checked: Tk.panels.kind === "tabs"
                    onTriggered: Tk.arrangePanels("tabs")
                }
            }
            Action {
                objectName: "action-layout-swap-panels"
                text: "Swap panels"
                enabled: Tk.panels.editing ?? false
                onTriggered: Tk.swapPanels()
            }
            Action {
                objectName: "action-reset-panel-layout"
                text: "Reset panel layout"
                onTriggered: Tk.resetPanels()
            }
        }
        Menu {
            title: "&Playback"
            KeyedAction {
                objectName: "action-play-pause"
                text: Tk.transport.playLabel ?? "Play"
                defaultKey: "Space"
                enabled: Tk.transport.canPlayPause ?? false
                onTriggered: Tk.playPause()
            }
            KeyedAction {
                objectName: "action-stop"
                text: "Stop"
                defaultKey: "Ctrl+."
                enabled: Tk.transport.canStop ?? false
                onTriggered: Tk.stop()
            }
            KeyedAction {
                objectName: "action-previous-track"
                text: "Previous"
                defaultKey: "Alt+Left"
                enabled: Tk.transport.canPrevious ?? false
                onTriggered: Tk.previous()
            }
            KeyedAction {
                objectName: "action-next-track"
                text: "Next"
                defaultKey: "Alt+Right"
                enabled: Tk.transport.canNext ?? false
                onTriggered: Tk.next()
            }
            MenuSeparator {}
            KeyedAction {
                objectName: "action-local-repeat"
                text: "Repeat"
                checkable: true
                checked: Tk.modes.repeat?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setRepeat(checked)
            }
            KeyedAction {
                objectName: "action-local-random"
                text: "Random"
                checkable: true
                checked: Tk.modes.random?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setRandom(checked)
            }
            KeyedAction {
                objectName: "action-local-single"
                text: Tk.modes.single?.text ?? "Single"
                checkable: true
                checked: Tk.modes.single?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.cycleSingle()
            }
            KeyedAction {
                objectName: "action-local-album-random"
                text: qsTr("Album shuffle")
                checkable: true
                checked: Tk.modes.albumRandom?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setAlbumRandom(checked)
            }
            KeyedAction {
                objectName: "action-local-consume"
                text: Tk.modes.consume?.text ?? "Consume"
                checkable: true
                checked: Tk.modes.consume?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.cycleConsume()
            }
            ReplayGainMenu {
                onPreampRequested: window.openSettings("playback").focusReplayGainPreamp()
            }
            MenuSeparator {}
            Action {
                text: "Desktop notifications"
                checkable: true
                checked: Tk.notifications
                onTriggered: Tk.notifications = checked
            }
            Menu {
                id: bufferMenu
                title: "Playback buffer"
                Instantiator {
                    model: Tk.bufferProfiles()
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData.label
                        checkable: true
                        checked: Tk.bufferProfile === modelData.value
                        ToolTip.visible: hovered
                        ToolTip.text: modelData.tooltip
                        onTriggered: Tk.setBufferProfile(modelData.value)
                    }
                    onObjectAdded: (index, object) => bufferMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => bufferMenu.takeItem(index)
                }
                MenuSeparator {}
                MenuItem {
                    text: "Custom…"
                    checkable: true
                    checked: Tk.bufferProfile === "custom"
                    onTriggered: window.openSettings("playback").editCustomBuffer()
                }
            }
            KeyedAction {
                objectName: "action-refresh-audio-devices"
                text: "Refresh audio devices"
                onTriggered: Tk.refreshOutputs()
            }
        }
    }

    header: TransportBar {
        upNextShown: upNextSettings.visible
        onToggleUpNext: upNextSettings.visible = !upNextSettings.visible
    }

    footer: ColumnLayout {
        spacing: 0
        PlaylistTransferBar {
            Layout.fillWidth: true
        }
        StatusRow {
            id: status
            Layout.fillWidth: true
        }
    }

    Shortcut {
        sequences: ["Ctrl+Y"]
        onActivated: Tk.redoListEdit()
    }
    // Commands with a key and no menu entry: in Settings › Shortcuts too.
    readonly property var keyedCommands: [
        {id: "action-queue-next", label: qsTr("Queue next"), key: "Ctrl+Return"},
        {id: "action-queue-end", label: qsTr("Queue at end"), key: "Ctrl+Shift+Return"},
        {id: "action-focus-library-search", label: qsTr("Search the library"), key: "Ctrl+L"},
        {id: "action-show-up-next", label: qsTr("Up Next"), key: "Ctrl+Shift+U"}
    ]
    function keyOf(id) {
        const command = keyedCommands.find(entry => entry.id === id);
        return Tk.shortcutRevision >= 0 ? Tk.shortcut(id, command.key) : "";
    }
    Shortcut {
        sequence: window.keyOf("action-queue-next")
        onActivated: Tk.queueSelection(true)
    }
    Shortcut {
        sequence: window.keyOf("action-queue-end")
        onActivated: Tk.queueSelection(false)
    }
    Shortcut {
        sequence: window.keyOf("action-focus-library-search")
        onActivated: Tk.focusLibrarySearch()
    }
    Shortcut {
        sequence: window.keyOf("action-show-up-next")
        onActivated: upNextSettings.visible = !upNextSettings.visible
    }
    property var dynamicWindow: null
    function openDynamic() {
        if (dynamicWindow) {
            Tk.followDynamic(dynamicWindow.dynamic);
            dynamicWindow.raise();
            dynamicWindow.requestActivate();
            return;
        }
        dynamicWindow = dynamicComponent.createObject(window, {dynamic: Tk.openDynamic()});
    }
    property var searchWindow: null
    function openSearch() {
        if (searchWindow) {
            Tk.followSearch(searchWindow.search);
            searchWindow.raise();
            searchWindow.requestActivate();
            searchWindow.focusInput();
            return;
        }
        const search = Tk.openSearch();
        if (!search)
            return;
        searchWindow = searchComponent.createObject(window, {search: search});
        searchWindow.focusInput();
    }
    function openQuickPick(albums) {
        const pick = Tk.openQuickPick(albums);
        if (!pick)
            return;
        quickPickPopup.pick = pick;
        quickPickPopup.open();
    }
    // The workspace commands the command palette offers, as they stand.
    function paletteCommands() {
        const ids = Tk.workspaceCommandIds();
        const listed = [];
        const seen = {};
        function walk(menu) {
            for (let i = 0; i < menu.count; ++i) {
                const submenu = menu.menuAt(i);
                if (submenu) {
                    walk(submenu);
                    continue;
                }
                const action = menu.actionAt(i);
                if (!action || seen[action.objectName] || !ids.includes(action.objectName))
                    continue;
                seen[action.objectName] = true;
                listed.push({id: action.objectName, label: action.text.replace(/&/g, "").trim(),
                             shortcut: Tk.nativeShortcut(String(action.shortcut ?? "")),
                             enabled: action.enabled, checked: action.checkable && action.checked,
                             run: () => action.trigger()});
            }
        }
        for (let i = 0; i < window.menuBar.count; ++i)
            walk(window.menuBar.menuAt(i));
        const runs = {"action-focus-library-search": () => Tk.focusLibrarySearch(),
                      "action-show-up-next": () => upNextSettings.visible = !upNextSettings.visible};
        for (const command of keyedCommands) {
            if (runs[command.id] && !seen[command.id])
                listed.push({id: command.id, label: command.label,
                             shortcut: Tk.nativeShortcut(window.keyOf(command.id)),
                             enabled: true, checked: command.id === "action-show-up-next" && upNextSettings.visible,
                             run: runs[command.id]});
        }
        return listed.sort((a, b) => a.label.localeCompare(b.label));
    }
    // Every command a key can be given: the menus' and those above.
    function shortcutCommands() {
        const listed = [];
        const seen = {};
        function walk(menu) {
            for (let i = 0; i < menu.count; ++i) {
                const submenu = menu.menuAt(i);
                if (submenu) {
                    walk(submenu);
                    continue;
                }
                const action = menu.actionAt(i);
                if (action && action.objectName.startsWith("action-") && !seen[action.objectName]) {
                    seen[action.objectName] = true;
                    listed.push({id: action.objectName, label: action.text,
                                 key: action.defaultKey ?? ""});
                }
            }
        }
        for (let i = 0; i < window.menuBar.count; ++i)
            walk(window.menuBar.menuAt(i));
        return listed.concat(keyedCommands);
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // The two panels, arranged as Workspace › Panel arrangement says:
        // side by side, one above the other, or as tabs; in either order.
        Item {
            id: panelsHost
            SplitView.fillWidth: true
            SplitView.minimumWidth: 360

            readonly property var panels: Tk.panels
            readonly property bool tabbed: (panels.kind ?? "split") === "tabs"
            readonly property var order: panels.order ?? ["folders", "track-lists"]
            function panelOf(id) {
                return id === "folders" ? sources : trackArea;
            }
            // The panels in the order and room kept.
            function arrange() {
                const first = panelOf(order[0]);
                if (panelSplit.itemAt(0) !== first)
                    panelSplit.moveItem(1, 0);
                const weights = panels.weights ?? [1, 3];
                const total = weights.reduce((sum, weight) => sum + weight, 0) || 1;
                const extent = panels.vertical ? panelSplit.height : panelSplit.width;
                for (let index = 0; index < order.length; ++index) {
                    const panel = panelOf(order[index]);
                    const size = Math.max(1, Math.round(extent * (weights[index] ?? 1) / total));
                    if (panels.vertical)
                        panel.SplitView.preferredHeight = size;
                    else
                        panel.SplitView.preferredWidth = size;
                }
            }
            onPanelsChanged: Qt.callLater(arrange)
            onWidthChanged: if (!panelSplit.resizing) Qt.callLater(arrange)
            onHeightChanged: if (!panelSplit.resizing) Qt.callLater(arrange)
            Component.onCompleted: Qt.callLater(arrange)

            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                TabBar {
                    id: panelTabs
                    Layout.fillWidth: true
                    visible: panelsHost.tabbed
                    currentIndex: panelsHost.panels.active ?? 0
                    onCurrentIndexChanged: if (panelsHost.tabbed) Tk.setPanelTab(currentIndex)
                    Repeater {
                        model: panelsHost.order
                        TabButton {
                            required property string modelData
                            text: modelData === "folders" ? qsTr("Sources") : qsTr("Lists and tracks")
                        }
                    }
                }
                SplitView {
                    id: panelSplit
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    orientation: (panelsHost.panels.vertical ?? false) ? Qt.Vertical : Qt.Horizontal
                    // A divider moved by hand: the room each has is kept.
                    onResizingChanged: {
                        if (resizing)
                            return;
                        const sizes = [];
                        for (const id of panelsHost.order) {
                            const panel = panelsHost.panelOf(id);
                            sizes.push(orientation === Qt.Vertical ? panel.height : panel.width);
                        }
                        Tk.setPanelSizes(sizes);
                    }

                    // bench-panel-folders: Sources.
                    SourcesPanel {
                        id: sources
                        visible: !panelsHost.tabbed || panelsHost.order[panelTabs.currentIndex] === "folders"
                        SplitView.minimumWidth: 160
                        SplitView.minimumHeight: 120
                        onNotYet: what => window.notYet(what)
                        Rectangle {
                            anchors.fill: parent
                            visible: panelsHost.panels.editing ?? false
                            color: "transparent"
                            border.color: palette.highlight
                            border.width: 1
                        }
                    }

                    // bench-track-area: the tabs over the list on show, and
                    // the lists beside it when they are a pane.
                    SplitView {
                        id: trackArea
                        visible: !panelsHost.tabbed || panelsHost.order[panelTabs.currentIndex] === "track-lists"
                        SplitView.fillWidth: true
                        SplitView.fillHeight: true
                        SplitView.minimumWidth: 200
                        SplitView.minimumHeight: 160
                        orientation: Qt.Horizontal

                        ColumnLayout {
                            SplitView.fillWidth: true
                            SplitView.minimumWidth: 200
                            spacing: 0
                            TrackTabBar {
                                Layout.fillWidth: true
                                visible: !Tk.listsInPanel
                                onContextMenuRequested: (index, position) => tabMenu.popup()
                                onNewListRequested: nameDialog.ask("New list", "", name => Tk.newList(name))
                            }
                            TrackTable {
                                id: trackTable
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                onContextMenuRequested: (row, position) => trackMenu.popup()
                                onHeaderMenuRequested: position => headerMenu.popup()
                            }
                            FindBar {
                                Layout.fillWidth: true
                            }
                            EditBar {
                                id: editBar
                                Layout.fillWidth: true
                            }
                        }
                        ListsPane {
                            visible: Tk.listsInPanel
                            SplitView.minimumWidth: 160
                            SplitView.preferredWidth: Tk.listsPanelWidth()
                            onWidthChanged: if (visible && trackArea.resizing) Tk.setListsPanelWidth(width)
                            onNewListRequested: nameDialog.ask("New list", "", name => Tk.newList(name))
                            onRenameRequested: (engine, id, name) => nameDialog.ask("Rename list", name,
                                                   chosen => Tk.renamePanelList(engine, id, chosen))
                        }
                        Rectangle {
                            parent: trackArea
                            anchors.fill: parent
                            z: 10
                            visible: panelsHost.panels.editing ?? false
                            color: "transparent"
                            border.color: palette.highlight
                            border.width: 1
                        }
                    }
                }
            }
        }

        UpNextPanel {
            visible: upNextSettings.visible
            SplitView.minimumWidth: 260
            SplitView.maximumWidth: Math.max(260, window.width / 2)
            SplitView.preferredWidth: upNextSettings.width
            onWidthChanged: if (visible && width >= 260)
                upNextSettings.width = width
            onCloseRequested: upNextSettings.visible = false
        }
    }

    // Tab context: Rename, Save, Pin, Duplicate, then Close.
    Menu {
        id: tabMenu
        MenuItem { action: renameAction }
        MenuItem { action: saveAction }
        MenuItem { action: pinAction }
        MenuItem { action: duplicateAction }
        MenuSeparator {}
        MenuItem { action: closeAction }
    }

    TrackContextMenu {
        id: trackMenu
        onNotYet: what => window.notYet(what)
        onCustomSortRequested: editBar.openSort()
        onNewTabRequested: move => nameDialog.ask("New tab", "Selection",
                                                  name => Tk.transferSelectionToNewTab(name, move))
    }

    TrackLayoutMenu {
        id: headerMenu
        headerMenu: true
    }

    Dialog {
        id: nameDialog
        property var accept: null
        function ask(title, current, then) {
            nameDialog.title = title;
            nameField.text = current;
            accept = then;
            open();
            nameField.selectAll();
            nameField.forceActiveFocus();
        }
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        RowLayout {
            Label {
                text: "Name:"
            }
            TextField {
                id: nameField
                Layout.preferredWidth: 280
                onAccepted: nameDialog.accept()
            }
        }
        onAccepted: {
            if (accept && nameField.text.trim() !== "")
                accept(nameField.text);
        }
    }

    MessageDialog {
        id: discardDialog
        property int index: -1
        title: "Close unsaved list"
        buttons: MessageDialog.Yes | MessageDialog.No
        onButtonClicked: (button, role) => {
            if (button === MessageDialog.Yes)
                Tk.closeTab(index);
        }
    }

    MessageDialog {
        id: informationDialog
        buttons: MessageDialog.Ok
    }

    FileDialog {
        id: filesDialog
        title: "Open files"
        fileMode: FileDialog.OpenFiles
        onAccepted: Tk.openUrls(selectedFiles)
    }

    FolderDialog {
        id: folderDialog
        title: "Open folder"
        onAccepted: Tk.openUrls([selectedFolder])
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import M3U8 into a new local list")
        nameFilters: [qsTr("UTF-8 playlists (*.m3u8)")]
        fileMode: FileDialog.OpenFile
        onAccepted: Tk.importPlaylist(selectedFile)
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export M3U8 to a new file")
        nameFilters: [qsTr("UTF-8 playlists (*.m3u8)")]
        defaultSuffix: "m3u8"
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontConfirmOverwrite
        onAccepted: Tk.exportPlaylist(selectedFile)
    }
    FileDialog {
        id: backupDialog
        title: qsTr("Back up Trackknife workspace database")
        nameFilters: [qsTr("Trackknife workspace database (*.sqlite)")]
        defaultSuffix: "sqlite"
        fileMode: FileDialog.SaveFile
        options: FileDialog.DontConfirmOverwrite
        currentFile: "trackknife-workspace.sqlite"
        onAccepted: Tk.backupWorkspace(selectedFile)
    }
    FileDialog {
        id: restoreDialog
        title: qsTr("Restore Trackknife workspace database")
        nameFilters: [qsTr("Trackknife workspace database (*.sqlite)"), qsTr("All files (*)")]
        fileMode: FileDialog.OpenFile
        onAccepted: restoreConfirm.open()
    }
    MessageDialog {
        id: restoreConfirm
        title: qsTr("Restore workspace database")
        text: qsTr("Trackknife will validate and restore this database at the next start. The current database will be retained beside it for rollback. Close Trackknife now?")
        buttons: MessageDialog.Close | MessageDialog.Cancel
        onButtonClicked: (button, role) => {
            if (button === MessageDialog.Close) {
                Tk.scheduleWorkspaceRestore(restoreDialog.selectedFile);
                window.close();
            }
        }
    }
    FolderDialog {
        id: bookmarkDialog
        title: qsTr("Bookmark folder")
        onAccepted: Tk.bookmarkFolder(selectedFolder)
    }
    QuickPickPopup {
        id: quickPickPopup
        parent: Overlay.overlay
    }
    CommandPalette {
        id: commandPaletteDialog
    }
    Component {
        id: dynamicComponent
        DynamicPlaylistsWindow {
            Component.onDestruction: window.dynamicWindow = null
        }
    }
    Component {
        id: searchComponent
        SearchWindow {
            Component.onDestruction: window.searchWindow = null
        }
    }
    property var openListWindow: null
    Component {
        id: openListComponent
        OpenListDialog {
            Component.onDestruction: window.openListWindow = null
        }
    }

    Connections {
        target: Tk
        function onSaveListWanted() {
            saveAction.trigger();
        }
        function onMessage(text, timeoutMs) {
            status.showMessage(text, timeoutMs);
        }
        function onInformation(title, text) {
            informationDialog.title = title;
            informationDialog.text = text;
            informationDialog.open();
        }
    }

    onClosing: Tk.closeWindow()

    // Each "Edit tags" is a window of its own (ADR-0221): several can stand
    // open over different selections.
    Component {
        id: taggerComponent
        TaggerWindow {
            id: taggerWindow
            onSettingsRequested: page => {
                const opened = window.openSettings(page);
                if (page === "naming")
                    opened.showNamingLayouts();
            }
            // Managing destinations opens those of the engine its tracks
            // are on.
            onDestinationsRequested: window.openSettings("naming").showDestinationsOf(taggerWindow.tagger.engineKey)
        }
    }
    Component {
        id: settingsComponent
        SettingsWindow {
            Component.onDestruction: window.settingsWindow = null
        }
    }
    Component {
        id: replayGainComponent
        ReplayGainDialog {}
    }
    Component {
        id: convertComponent
        ConvertDialog {}
    }
    Connections {
        target: Tk
        function onConvertOpened(convert) {
            const opened = convertComponent.createObject(window, {convert: convert});
            opened.raise();
            opened.requestActivate();
        }
        function onReplayGainOpened(replayGain) {
            const opened = replayGainComponent.createObject(window, {replayGain: replayGain});
            if (window.replayGainForScreenshot)
                replayGain.preview();
            opened.raise();
            opened.requestActivate();
        }
        function onTaggerOpened(tagger) {
            const opened = taggerComponent.createObject(window, {tagger: tagger});
            opened.raise();
            opened.requestActivate();
            // QA hook only: a picture of the editor with an edit staged.
            if (window.taggerForScreenshot.startsWith("tagger")
                    && window.taggerForScreenshot !== "tagger")
                Qt.callLater(() => stageTimer.start());
            stageTimer.opened = opened;
        }
    }
}
