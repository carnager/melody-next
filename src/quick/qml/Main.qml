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
        else if (name === "search" && Tk.library)
            Tk.library.search = "a";
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
            Action {
                objectName: "action-new-list"
                text: "New list…"
                shortcut: "Ctrl+N"
                onTriggered: nameDialog.ask("New list", "", name => Tk.newList(name))
            }
            Action {
                objectName: "action-open-files"
                text: "Open files…"
                shortcut: "Ctrl+O"
                onTriggered: filesDialog.open()
            }
            Action {
                objectName: "action-open-folder"
                text: "Open folder…"
                shortcut: "Ctrl+Shift+O"
                onTriggered: folderDialog.open()
            }
            Action {
                objectName: "action-import-m3u8"
                text: qsTr("Import M3U8 playlist…")
                onTriggered: window.notYet("Importing a playlist")
            }
            Action {
                objectName: "action-export-m3u8"
                text: qsTr("Export list as M3U8…")
                enabled: Tk.currentTab >= 0
                onTriggered: window.notYet("Exporting a playlist")
            }
            Action {
                objectName: "action-open-list"
                text: "Open list…"
                shortcut: "Ctrl+Alt+O"
                onTriggered: window.notYet("Open list")
            }
            Action {
                objectName: "action-dynamic-playlists"
                text: "Dynamic playlists…"
                onTriggered: window.notYet("Dynamic playlists")
            }
            Action {
                objectName: "action-backup-workspace"
                text: "Back up workspace database…"
                onTriggered: window.notYet("Backing up the workspace")
            }
            Action {
                objectName: "action-restore-workspace"
                text: "Restore workspace database…"
                onTriggered: window.notYet("Restoring the workspace")
            }
            Action {
                text: "Bookmark folder…"
                onTriggered: window.notYet("Bookmarks")
            }
            MenuSeparator {}
            Action {
                text: "Close window"
                onTriggered: window.close()
            }
            Action {
                objectName: "action-quit-stop-engine"
                text: "Quit and stop playback"
                shortcut: "Ctrl+Q"
                onTriggered: Tk.quitAndStopEngine()
            }
        }
        Menu {
            title: "&Edit"
            Action {
                text: qsTr("Find in current list…")
                shortcut: "Ctrl+F"
                enabled: Tk.currentTab >= 0
                onTriggered: window.notYet("Find in list")
            }
            Action {
                text: qsTr("Find next in list")
                shortcut: "F3"
                onTriggered: window.notYet("Find in list")
            }
            Action {
                text: qsTr("Find previous in list")
                shortcut: "Shift+F3"
                onTriggered: window.notYet("Find in list")
            }
            MenuSeparator {}
            Action {
                objectName: "action-undo-list-edit"
                text: Tk.history.undoText ?? "Undo list edit"
                shortcut: "Ctrl+Z"
                enabled: Tk.history.canUndo ?? false
                onTriggered: Tk.undoListEdit()
            }
            Action {
                objectName: "action-redo-list-edit"
                text: Tk.history.redoText ?? "Redo list edit"
                shortcut: "Ctrl+Shift+Z"
                enabled: Tk.history.canRedo ?? false
                onTriggered: Tk.redoListEdit()
            }
            MenuSeparator {}
            SortMenu {
                onCustomRequested: editBar.openSort()
            }
            Action {
                objectName: "action-reverse-list"
                text: qsTr("Reverse list")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.reverse()
            }
            Action {
                objectName: "action-shuffle-albums"
                text: qsTr("Shuffle albums")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.shuffleAlbums()
            }
            Action {
                objectName: "action-deduplicate-list"
                text: qsTr("Remove duplicate entries")
                enabled: Tk.history.editable ?? false
                onTriggered: Tk.edit.removeDuplicates()
            }
            MenuSeparator {}
            Action {
                objectName: "action-edit-tags"
                text: "Edit tags…"
                shortcut: "Alt+Return"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: window.notYet("The tagger")
            }
            Action {
                objectName: "action-replaygain"
                text: "ReplayGain…"
                onTriggered: window.notYet("ReplayGain scanning")
            }
            Action {
                objectName: "action-convert"
                text: "Convert files…"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: window.notYet("Converting")
            }
            MenuSeparator {}
            Action {
                objectName: "action-settings"
                text: "Settings…"
                shortcut: "Ctrl+,"
                onTriggered: window.notYet("Settings")
            }
            MenuSeparator {}
            Action {
                objectName: "action-remove-selected"
                text: "Remove selected"
                shortcut: "Delete"
                enabled: (Tk.selection.count ?? 0) > 0
                onTriggered: Tk.removeSelectedRows()
            }
        }
        Menu {
            title: "&Workspace"
            Action {
                text: qsTr("Commands…")
                shortcut: "Ctrl+Shift+P"
                onTriggered: window.notYet("The command palette")
            }
            MenuSeparator {}
            Action {
                objectName: "action-jump-to-playing"
                text: qsTr("Jump to playing")
                shortcut: "Ctrl+J"
                onTriggered: Tk.jumpToPlaying()
            }
            Action {
                objectName: "action-follow-playback"
                text: qsTr("Cursor follows playback")
                shortcut: "Ctrl+Shift+J"
                checkable: true
                checked: Tk.followPlayback
                onTriggered: Tk.followPlayback = checked
            }
            MenuSeparator {}
            Action {
                text: "Search…"
                shortcut: "Ctrl+Shift+F"
                onTriggered: window.notYet("Search")
            }
            Action {
                text: qsTr("Quick album…")
                shortcut: "Ctrl+Shift+A"
                onTriggered: window.notYet("Quick album")
            }
            Action {
                text: qsTr("Quick track…")
                shortcut: "Ctrl+Shift+T"
                onTriggered: window.notYet("Quick track")
            }
            Action {
                objectName: "action-lists-panel"
                text: qsTr("Lists in a side panel")
                checkable: true
                onTriggered: {
                    checked = false;
                    window.notYet("The lists panel");
                }
            }
            MenuSeparator {}
            Action {
                id: duplicateAction
                text: "Duplicate tab"
                shortcut: "Ctrl+Shift+D"
                enabled: Tk.currentTab >= 0
                onTriggered: Tk.duplicateTab()
            }
            Action {
                id: pinAction
                text: "Pin tab"
                shortcut: "Ctrl+Alt+P"
                checkable: true
                checked: Tk.list.pinned ?? false
                enabled: Tk.currentTab >= 0
                onTriggered: Tk.togglePinned()
            }
            Action {
                id: saveAction
                text: "Save list"
                shortcut: "Ctrl+S"
                enabled: Tk.currentTab >= 0
                onTriggered: {
                    if (Tk.list.scratch)
                        nameDialog.ask("Save working list", Tk.list.name, name => Tk.saveTab(name));
                    else
                        Tk.saveTab("");
                }
            }
            Action {
                id: renameAction
                text: "Rename tab…"
                shortcut: "F2"
                enabled: Tk.currentTab >= 0
                onTriggered: nameDialog.ask("Rename list", Tk.list.name, name => Tk.renameTab(name))
            }
            MenuSeparator {}
            Action {
                id: closeAction
                text: "Close tab"
                shortcut: "Ctrl+W"
                enabled: Tk.currentTab >= 0
                onTriggered: window.closeTab(Tk.currentTab)
            }
            MenuSeparator {}
            TrackLayoutMenu {}
            MenuSeparator {}
            Action {
                text: "Edit panel layout"
                shortcut: "Ctrl+Alt+L"
                checkable: true
                onTriggered: {
                    checked = false;
                    window.notYet("Editing the panel layout");
                }
            }
            Menu {
                title: "Panel arrangement"
                enabled: false
                Action { text: "Side by side" }
                Action { text: "Top and bottom" }
                Action { text: "Tabbed stack" }
            }
            Action {
                text: "Swap panels"
                enabled: false
            }
            Action {
                text: "Reset panel layout"
                onTriggered: window.notYet("Panel layouts")
            }
        }
        Menu {
            title: "&Playback"
            Action {
                objectName: "action-play-pause"
                text: Tk.transport.playLabel ?? "Play"
                shortcut: "Space"
                enabled: Tk.transport.canPlayPause ?? false
                onTriggered: Tk.playPause()
            }
            Action {
                objectName: "action-stop"
                text: "Stop"
                shortcut: "Ctrl+."
                enabled: Tk.transport.canStop ?? false
                onTriggered: Tk.stop()
            }
            Action {
                objectName: "action-previous-track"
                text: "Previous"
                shortcut: "Alt+Left"
                enabled: Tk.transport.canPrevious ?? false
                onTriggered: Tk.previous()
            }
            Action {
                objectName: "action-next-track"
                text: "Next"
                shortcut: "Alt+Right"
                enabled: Tk.transport.canNext ?? false
                onTriggered: Tk.next()
            }
            MenuSeparator {}
            Action {
                objectName: "action-local-repeat"
                text: "Repeat"
                checkable: true
                checked: Tk.modes.repeat?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setRepeat(checked)
            }
            Action {
                objectName: "action-local-random"
                text: "Random"
                checkable: true
                checked: Tk.modes.random?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setRandom(checked)
            }
            Action {
                objectName: "action-local-single"
                text: Tk.modes.single?.text ?? "Single"
                checkable: true
                checked: Tk.modes.single?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.cycleSingle()
            }
            Action {
                objectName: "action-local-album-random"
                text: qsTr("Album shuffle")
                checkable: true
                checked: Tk.modes.albumRandom?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.setAlbumRandom(checked)
            }
            Action {
                objectName: "action-local-consume"
                text: Tk.modes.consume?.text ?? "Consume"
                checkable: true
                checked: Tk.modes.consume?.checked ?? false
                enabled: Tk.modes.enabled ?? false
                onTriggered: Tk.cycleConsume()
            }
            ReplayGainMenu {
                onPreampRequested: window.notYet("The preamp settings")
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
                    onTriggered: window.notYet("A custom buffer")
                }
            }
            Action {
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

    footer: StatusRow {
        id: status
    }

    Shortcut {
        sequences: ["Ctrl+Y"]
        onActivated: Tk.redoListEdit()
    }
    Shortcut {
        sequence: "Ctrl+Return"
        onActivated: Tk.queueSelection(true)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Return"
        onActivated: Tk.queueSelection(false)
    }
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: Tk.focusLibrarySearch()
    }
    Shortcut {
        sequence: "Ctrl+Shift+U"
        onActivated: upNextSettings.visible = !upNextSettings.visible
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // bench-panel-folders: Sources.
        SourcesPanel {
            SplitView.minimumWidth: 160
            SplitView.preferredWidth: (window.width - (upNextSettings.visible ? upNextSettings.width : 0)) / 4
            onNotYet: what => window.notYet(what)
        }

        // bench-track-area: the tabs over the list on show.
        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 200
            spacing: 0
            TrackTabBar {
                Layout.fillWidth: true
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
            EditBar {
                id: editBar
                Layout.fillWidth: true
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

    Connections {
        target: Tk
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
}
