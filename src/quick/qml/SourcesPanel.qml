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

        // bench-local-source-tabs: plain text, the chosen one filled with the
        // ground of what it shows below, a hovered one faintly.
        Row {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 4
            spacing: 2
            Accessible.name: "Local music source"
            Repeater {
                model: Tk.sources
                delegate: AbstractButton {
                    id: tab
                    required property var modelData
                    required property int index
                    readonly property bool current: index === Tk.currentSource
                    topInset: 3
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 5 + 3
                    bottomPadding: 5
                    hoverEnabled: true
                    text: modelData.title
                    ToolTip.visible: hovered && (modelData.tooltip ?? "") !== ""
                    ToolTip.text: modelData.tooltip ?? ""
                    background: Item {
                        clip: true
                        Rectangle {
                            width: parent.width
                            height: parent.height + 6
                            radius: 5
                            visible: tab.current || tab.hovered
                            color: tab.current ? panel.palette.base
                                               : Shade.alpha(panel.palette.base, 110 / 255)
                        }
                    }
                    contentItem: Label {
                        text: tab.text
                        color: tab.current ? panel.palette.text : panel.palette.placeholderText
                    }
                    onClicked: Tk.selectSource(index, true)
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
            }
        }
    }
}
