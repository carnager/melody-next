// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Trackknife.Quick
import Trackknife.Style

// Sources (bench-panel-folders): a tab per source -- Folders, this
// computer's Library, each engine elsewhere's -- the bookmarks under
// Folders, and what the chosen source shows.
Pane {
    id: panel

    signal notYet(string what)
    signal foldersRequested()
    signal nameRequested(string title, string current, var then)

    padding: 0

    function openArtistForScreenshot() {
        libraryPane.toggleRowForScreenshot(0);
    }
    function openRowForScreenshot(row) {
        libraryPane.toggleRowForScreenshot(row);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // bench-local-source-tabs: one box split into its sources, the
        // chosen one lifted out of it; the lift slides to a source chosen.
        Rectangle {
            id: switcher
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 6
            Layout.bottomMargin: 4
            implicitHeight: 26
            radius: Theme.radius + 1
            color: panel.palette.base
            border.width: 1
            border.color: Theme.hairline(panel.palette)
            Accessible.role: Accessible.PageTabList
            Accessible.name: "Local music source"

            readonly property int count: Math.max(1, Tk.sources.length)
            readonly property real segment: (width - 4) / count

            Rectangle {
                id: lift
                x: 2 + Tk.currentSource * switcher.segment
                y: 2
                width: switcher.segment
                height: switcher.height - 4
                radius: Theme.radius
                color: Theme.raised(panel.palette)
                border.width: 1
                border.color: Theme.hairline(panel.palette)
                Behavior on x {
                    enabled: Tk.panelAnimations
                    NumberAnimation {
                        duration: Theme.moderate
                        easing.type: Easing.OutCubic
                    }
                }
            }
            Row {
                x: 2
                y: 2
                Repeater {
                    model: Tk.sources
                    delegate: AbstractButton {
                        id: tab
                        required property var modelData
                        required property int index
                        readonly property bool current: index === Tk.currentSource
                        width: switcher.segment
                        height: switcher.height - 4
                        hoverEnabled: true
                        text: modelData.title
                        Accessible.role: Accessible.PageTab
                        ToolTip.visible: hovered && (modelData.tooltip ?? "") !== ""
                        ToolTip.text: modelData.tooltip ?? ""
                        background: null
                        contentItem: Label {
                            text: tab.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            font.pointSize: Theme.smallSize
                            color: tab.current || tab.hovered ? panel.palette.windowText
                                                              : Theme.dim(panel.palette)
                            Behavior on color {
                                ColorAnimation { duration: Theme.quick }
                            }
                        }
                        onClicked: Tk.selectSource(index, true)
                    }
                }
            }
        }

        // Bookmarks, under Folders only.
        Label {
            objectName: "bench-folder-bookmarks-heading"
            visible: Tk.currentSource === 0 && Tk.folders.bookmarks.length > 0
            Layout.leftMargin: 8
            Layout.topMargin: 4
            Layout.bottomMargin: 2
            text: "Bookmarks"
            font.bold: true
            font.pointSize: Qt.application.font.pointSize * 0.85
        }
        ListView {
            id: bookmarks
            objectName: "bench-folder-bookmarks"
            visible: Tk.currentSource === 0 && count > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(150, contentHeight)
            clip: true
            model: Tk.folders.bookmarks
            Accessible.name: "Folder bookmarks"
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: bookmarks.width
                text: modelData.label
                leftPadding: 30
                FolderGlyph {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.dim(palette)
                }
                ToolTip.visible: hovered
                ToolTip.text: modelData.tooltip
                onClicked: Tk.folders.revealBookmark(index)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        bookmarkMenu.row = index;
                        bookmarkMenu.popup();
                    }
                }
            }
        }
        Menu {
            id: bookmarkMenu
            objectName: "bench-folder-bookmark-menu"
            property int row: -1
            MenuItem {
                objectName: "action-folder-bookmark-remove"
                text: "Remove bookmark"
                onTriggered: Tk.folders.removeBookmark(bookmarkMenu.row)
            }
        }

        // bench-source-stack
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: Tk.currentSource === 0 ? 0 : 1
            FolderTree {}
            LibraryPane {
                id: libraryPane
                onFoldersRequested: panel.foldersRequested()
                onNameRequested: (title, current, then) => panel.nameRequested(title, current, then)
            }
        }
    }
}
