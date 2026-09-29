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
        property alias x: window.x
        property alias y: window.y
        property alias width: window.width
        property alias height: window.height
    }
    property bool upNextOpen: true

    Shortcut {
        sequence: "Space"
        onActivated: Engine.current.player.toggle()
    }
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: library.searchField.forceActiveFocus()
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
            title: "Workspace"
            MenuItem {
                text: "Up Next"
                checkable: true
                checked: window.upNextOpen
                onTriggered: window.upNextOpen = !window.upNextOpen
            }
        }
        MenuSeparator {}
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
                    SplitView.preferredWidth: 340
                    SplitView.minimumWidth: 220
                }
                TrackListPane {
                    id: trackList
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 400
                }
            }

            UpNextPane {
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

    // Until the engine answers.
    Rectangle {
        anchors.fill: parent
        visible: opacity > 0
        opacity: Engine.current.connected ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 200 } }
        color: Qt.rgba(0.08, 0.09, 0.11, 0.85)

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
