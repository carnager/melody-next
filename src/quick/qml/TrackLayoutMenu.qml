// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// "Track list layout": the four presentations -- choosing one replaces the
// list's columns with its defaults -- the columns, and resetting. As the
// header's menu it starts with a "Presentation" heading instead.
Menu {
    id: menu

    property bool headerMenu: false
    readonly property string presentation: Tk.list.presentation ?? ""
    readonly property var visibleIds: Tk.list.visibleColumns ?? []

    title: "Track list layout"

    MenuItem {
        visible: menu.headerMenu
        height: visible ? implicitHeight : 0
        enabled: false
        text: "Presentation"
        font.bold: true
    }
    Repeater {
        model: [
            {value: "albums-side-artwork", label: "Albums with side artwork"},
            {value: "albums-header-artwork", label: "Albums with header artwork"},
            {value: "plain-columns", label: "Plain columns"},
            {value: "compact-queue", label: "Compact queue"},
        ]
        delegate: MenuItem {
            required property var modelData
            text: modelData.label
            checkable: true
            checked: menu.presentation === modelData.value
            onTriggered: Tk.setPresentation(modelData.value)
        }
    }
    MenuSeparator {
        visible: !menu.headerMenu
    }
    Menu {
        title: "Columns"
        Repeater {
            model: [
                {id: "artwork", label: "Artwork"},
                {id: "artist", label: "Artist"},
                {id: "track-number", label: "Track number"},
                {id: "title", label: "Title"},
                {id: "album", label: "Album"},
                {id: "date", label: "Date"},
                {id: "length", label: "Length"},
                {id: "rating", label: "Rating"},
                {id: "play-count", label: "Play count"},
                {id: "last-played", label: "Last played"},
            ]
            delegate: MenuItem {
                required property var modelData
                text: modelData.label
                checkable: true
                checked: (menu.visibleIds ?? []).indexOf(modelData.id) >= 0
                onTriggered: Tk.setColumnVisible(modelData.id, checked)
            }
        }
    }
    MenuSeparator {
        visible: menu.headerMenu
    }
    MenuItem {
        text: "Reset current list layout"
        onTriggered: Tk.resetLayout()
    }
    MenuItem {
        visible: !menu.headerMenu
        height: visible ? implicitHeight : 0
        text: "Apply current layout to all queues and lists"
        onTriggered: Tk.copyLayoutToAll()
    }
}
