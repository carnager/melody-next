// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window

    width: 1600
    height: 950
    visible: true
    title: "Trackknife — Qt Quick mockup"
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

    Shortcut {
        sequence: "Space"
        onActivated: App.playing = !App.playing
    }
    Shortcut {
        sequence: "Ctrl+L"
        onActivated: {
            library.source = 1;
            library.searchField.forceActiveFocus();
        }
    }
    Shortcut {
        sequence: "Ctrl+T"
        onActivated: App.newTab()
    }

    Menu {
        id: appMenu
        Menu {
            title: "File"
            MenuItem { text: "Open files…" }
            MenuItem { text: "Add folder…" }
            MenuSeparator {}
            MenuItem { text: "Quit"; onTriggered: Qt.quit() }
        }
        Menu {
            title: "Edit"
            MenuItem { text: "Tag editor…" }
            MenuItem { text: "Scan ReplayGain…" }
            MenuItem { text: "Convert…" }
        }
        Menu {
            title: "Workspace"
            MenuItem {
                text: "Up Next"
                checkable: true
                checked: App.upNextOpen
                onTriggered: App.upNextOpen = !App.upNextOpen
            }
            MenuItem { text: "New list"; onTriggered: App.newTab() }
        }
        Menu {
            title: "Playback"
            MenuItem { text: App.playing ? "Pause" : "Play"; onTriggered: App.playing = !App.playing }
            MenuItem { text: "Next"; onTriggered: App.next() }
            MenuItem { text: "Previous"; onTriggered: App.previous() }
        }
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
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 400
                }
            }

            UpNextPane {
                Layout.fillHeight: true
                Layout.preferredWidth: App.upNextOpen ? 300 : 0
                Behavior on Layout.preferredWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            }
        }

        StatusBar {
            Layout.fillWidth: true
        }
    }
}
