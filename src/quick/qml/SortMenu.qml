// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// "Sort list": by title, artist/album/track, album/track, track number or
// path -- tkfmt-1 expressions -- or one of the user's own, in the edit bar.
Menu {
    id: sortMenu
    objectName: "bench-sort-list-menu"
    title: qsTr("Sort list")
    enabled: Tk.history.editable ?? false

    signal customRequested()

    Repeater {
        model: [
            {name: qsTr("Title"), object: "action-sort-list-title", expression: "%title%"},
            {name: qsTr("Artist / album / track"), object: "action-sort-list-artist",
             expression: "$if2(%albumartist%,%artist%)|%album%|%discnumber%|%tracknumber%|%title%"},
            {name: qsTr("Album / track"), object: "action-sort-list-album",
             expression: "%album%|%discnumber%|%tracknumber%|%title%"},
            {name: qsTr("Track number"), object: "action-sort-list-track",
             expression: "%discnumber%|%tracknumber%"},
            {name: qsTr("Path"), object: "action-sort-list-path", expression: "$info(path)"},
        ]
        delegate: MenuItem {
            required property var modelData
            objectName: modelData.object
            text: modelData.name
            onTriggered: Tk.edit.sort(modelData.expression, false)
        }
    }
    MenuSeparator {}
    MenuItem {
        objectName: "action-sort-list-custom"
        text: qsTr("Custom expression…")
        onTriggered: sortMenu.customRequested()
    }
}
