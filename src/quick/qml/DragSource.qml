// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick
import Trackknife.Style

// Makes what it fills draggable (ADR-0233): past the drag distance it says
// what it carries (`began`), shows `label` under the pointer and drags --
// moved by default, copied with Ctrl, only copied when `copyOnly`. Drops in
// this window and the others take it; files from elsewhere are URLs.
Item {
    id: source

    property string label
    property bool copyOnly: false
    // What the drag carries is said here, when it starts.
    signal began()
    readonly property bool dragging: handler.active

    anchors.fill: parent

    DragHandler {
        id: handler
        target: null
        acceptedButtons: Qt.LeftButton
        onActiveChanged: {
            if (!active)
                return;
            source.began();
            if (Tk.draggedKind() === "") {
                return;
            }
            badge.text = source.label;
            badge.grabToImage(result => {
                source.Drag.imageSource = result.url;
                source.Drag.active = true;
            });
        }
    }

    Drag.dragType: Drag.Automatic
    Drag.supportedActions: copyOnly ? Qt.CopyAction : Qt.MoveAction | Qt.CopyAction
    Drag.proposedAction: copyOnly ? Qt.CopyAction : Qt.MoveAction
    Drag.mimeData: ({"application/x-trackknife-drag": "1"})
    Drag.onDragFinished: dropAction => {
        source.Drag.active = false;
        Tk.endDrag();
    }

    // The badge under the pointer, drawn out of sight to be pictured.
    Label {
        id: badge
        x: -10000
        padding: 6
        leftPadding: 10
        rightPadding: 10
        color: palette.highlightedText
        background: Rectangle {
            radius: Theme.radius
            color: palette.highlight
        }
    }
}
