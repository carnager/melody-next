// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// What follows the pointer while something is dragged inside the window.
// One for the whole window, so every drop target reads the same payload:
//   {kind: "library", session, row}
//   {kind: "tracks", session, listId, rows}
//   {kind: "upnext", session, row}
// Tracks only ever go to lists of the engine they came from: a path means
// something on its own engine only.
Rectangle {
    id: ghost

    property var payload: null
    property string label

    function begin(payload, label, point) {
        ghost.payload = payload;
        ghost.label = label;
        moveTo(point);
        Drag.active = true;
    }

    function moveTo(point) {
        x = point.x + 14;
        y = point.y + 14;
    }

    function end() {
        if (Drag.active)
            Drag.drop();
        Drag.active = false;
        payload = null;
    }

    width: Math.min(360, text.implicitWidth + 28)
    height: 28
    radius: 6
    color: Theme.raised
    border.color: Theme.accent
    opacity: 0.95
    visible: Drag.active
    z: 1000

    Drag.dragType: Drag.Internal
    Drag.keys: ["trackknife"]
    // The pointer, not the ghost's corner, is what a drop area tests.
    Drag.hotSpot.x: -14
    Drag.hotSpot.y: -14

    Text {
        id: text
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        verticalAlignment: Text.AlignVCenter
        text: ghost.label
        color: Theme.text
        font.pixelSize: Theme.fontSize
        elide: Text.ElideRight
    }
}
