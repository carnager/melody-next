// SPDX-License-Identifier: GPL-3.0-only
import QtCore
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window

    width: 1600
    height: 950
    visible: true
    title: Engine.current.player.entry !== "" ? Engine.current.player.title + " — Trackknife" : "Trackknife"
    color: Theme.window

    palette.window: Theme.window
    palette.windowText: Theme.text
    palette.base: Theme.base
    palette.alternateBase: Theme.panel
    palette.text: Theme.text
    palette.button: Theme.raised
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: "white"
    palette.toolTipBase: Theme.raised
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.faint
    palette.mid: Theme.line
    palette.dark: Theme.window
    palette.light: Theme.raised
    font.pixelSize: Theme.fontSize

    Settings {
        // Its own file: the widgets window's settings are not this one's to write.
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/trackknife/quick-window.conf"
        category: "QuickWindow"
        property alias upNextOpen: window.upNextOpen
        property alias appearance: window.appearance
        property alias notifications: window.notifications
        property alias notifyInBackgroundOnly: window.notifyInBackgroundOnly
        property alias x: window.x
        property alias y: window.y
        property alias width: window.width
        property alias height: window.height
    }
    property bool upNextOpen: true
    // A quiet notification on each new track (ADR-0144); off unless asked.
    property bool notifications: false
    property bool notifyInBackgroundOnly: true
    Binding {
        target: Engine.desktop
        property: "notifications"
        value: window.notifications
    }
    Binding {
        target: Engine.desktop
        property: "backgroundOnly"
        value: window.notifyInBackgroundOnly
    }
    Connections {
        target: Engine.desktop
        function onRaiseRequested() {
            window.show();
            window.raise();
            window.requestActivate();
        }
    }

    // As Theme.mode: 0 the system's, 1 light, 2 dark.
    property int appearance: 0
    Binding {
        target: Theme
        property: "mode"
        value: window.appearance
    }

    Shortcut {
        sequence: "Space"
        onActivated: Engine.current.player.toggle()
    }
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: library.searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: StandardKey.Find
        onActivated: trackList.openFind()
    }
    Shortcut {
        sequences: ["F3"]
        onActivated: trackList.finding ? trackList.findStep(1, true) : trackList.openFind()
    }
    Shortcut {
        sequences: ["Shift+F3"]
        onActivated: trackList.finding ? trackList.findStep(-1, true) : trackList.openFind()
    }
    Shortcut {
        sequence: "Ctrl+J"
        onActivated: trackList.jumpToPlaying()
    }
    Shortcut {
        sequence: "Ctrl+Shift+J"
        onActivated: trackList.followPlayback = !trackList.followPlayback
    }
    Shortcut {
        sequence: StandardKey.New
        onActivated: trackList.newList(Engine.current)
    }
    Shortcut {
        sequence: StandardKey.Save
        onActivated: trackList.saveShown()
    }
    Shortcut {
        sequence: "F2"
        onActivated: trackList.renameShown()
    }
    Shortcut {
        sequence: "Ctrl+W"
        onActivated: trackList.closeShown()
    }
    Shortcut {
        sequence: "Ctrl+Shift+D"
        onActivated: trackList.duplicateShown()
    }
    Shortcut {
        sequence: "Ctrl+Shift+U"
        onActivated: window.upNextOpen = !window.upNextOpen
    }
    Shortcut {
        sequence: "Alt+Return"
        onActivated: trackList.editTags()
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsDialog.open()
    }
    Shortcut {
        sequence: StandardKey.Quit
        onActivated: Qt.quit()
    }

    Menu {
        id: appMenu
        Menu {
            title: "Playback"
            MenuItem { text: Engine.current.player.playing ? "Pause" : "Play"; onTriggered: Engine.current.player.toggle() }
            MenuItem { text: "Next"; onTriggered: Engine.current.player.next() }
            MenuItem { text: "Previous"; onTriggered: Engine.current.player.previous() }
        }
        Menu {
            title: "List"
            MenuItem { text: "New list"; onTriggered: trackList.newList(Engine.current) }
            MenuItem { text: "Rename…"; onTriggered: trackList.renameShown() }
            MenuItem { text: "Save as playlist…"; onTriggered: trackList.saveShown() }
            MenuItem { text: "Duplicate"; onTriggered: trackList.duplicateShown() }
            MenuItem { text: "Close tab"; onTriggered: trackList.closeShown() }
            MenuSeparator {}
            MenuItem { text: "Find in list"; onTriggered: trackList.openFind() }
            MenuItem { text: "Jump to what plays"; onTriggered: trackList.jumpToPlaying() }
            MenuItem {
                text: "Follow playback"
                checkable: true
                checked: trackList.followPlayback
                onTriggered: trackList.followPlayback = !trackList.followPlayback
            }
        }
        Menu {
            title: "Workspace"
            MenuItem {
                text: "Up Next"
                checkable: true
                checked: window.upNextOpen
                onTriggered: window.upNextOpen = !window.upNextOpen
            }
            MenuItem {
                text: "Notify on each new track"
                checkable: true
                checked: window.notifications
                onTriggered: window.notifications = !window.notifications
            }
            MenuItem {
                text: "Only while the window is in the background"
                checkable: true
                enabled: window.notifications
                checked: window.notifyInBackgroundOnly
                onTriggered: window.notifyInBackgroundOnly = !window.notifyInBackgroundOnly
            }
            Menu {
                title: "Appearance"
                MenuItem {
                    text: "As the system"
                    checkable: true
                    checked: window.appearance === 0
                    onTriggered: window.appearance = 0
                }
                MenuItem {
                    text: "Light"
                    checkable: true
                    checked: window.appearance === 1
                    onTriggered: window.appearance = 1
                }
                MenuItem {
                    text: "Dark"
                    checkable: true
                    checked: window.appearance === 2
                    onTriggered: window.appearance = 2
                }
            }
        }
        MenuSeparator {}
        MenuItem { text: "Settings…"; onTriggered: settingsDialog.open() }
        MenuItem { text: "Quit"; onTriggered: Qt.quit() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PlayerBar {
            Layout.fillWidth: true
            onMenuRequested: anchor => appMenu.popup(anchor, 0, anchor.height)
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            SplitView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                handle: Rectangle {
                    implicitWidth: 1
                    color: SplitHandle.hovered || SplitHandle.pressed ? Theme.accent : Theme.line
                }

                LibraryPane {
                    id: library
                    dragGhost: ghost
                    tagger: tagEditor
                    SplitView.preferredWidth: 340
                    SplitView.minimumWidth: 220
                }
                TrackListPane {
                    id: trackList
                    dragGhost: ghost
                    tagger: tagEditor
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 400
                }
            }

            UpNextPane {
                dragGhost: ghost
                Layout.fillHeight: true
                Layout.preferredWidth: window.upNextOpen ? 300 : 0
                Behavior on Layout.preferredWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                onCloseRequested: window.upNextOpen = false
            }
        }

        StatusBar {
            Layout.fillWidth: true
            text: trackList.summary
            upNextOpen: window.upNextOpen
            onUpNextToggled: window.upNextOpen = !window.upNextOpen
        }
    }

    DragGhost {
        id: ghost
    }

    TagEditorWindow {
        id: tagEditor
    }

    SettingsDialog {
        id: settingsDialog
        appWindow: window
    }
    // Remembered by the list pane; offered here so settings can reach it.
    property alias followPlayback: trackList.followPlayback

    // What an engine refused, said briefly and without a dialog.
    Instantiator {
        model: Engine.sessions
        delegate: Connections {
            required property var modelData
            target: modelData
            function onFailed(message) {
                toast.show(message);
            }
        }
    }

    Rectangle {
        id: toast
        function show(message) {
            toastText.text = message;
            opacity = 1;
            toastTimer.restart();
        }
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 44
        width: Math.min(parent.width - 40, toastText.implicitWidth + 32)
        height: 34
        radius: 8
        color: Theme.raised
        border.color: Theme.line
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: toastText
            anchors.centerIn: parent
            width: parent.width - 32
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: Theme.fontSize
        }
        Timer {
            id: toastTimer
            interval: 4000
            onTriggered: toast.opacity = 0
        }
    }

    // Until the engine answers.
    Rectangle {
        anchors.fill: parent
        visible: opacity > 0
        opacity: Engine.current.connected ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 200 } }
        color: Theme.scrim

        MouseArea { anchors.fill: parent }

        Column {
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Connecting to " + Engine.current.name + "…"
                color: Theme.text
                font.pixelSize: 16
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Engine.current.failure
                color: Theme.dim
                font.pixelSize: Theme.fontSize
            }
        }
    }
}
