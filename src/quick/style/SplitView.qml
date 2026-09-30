// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

// A hairline between panes, the accent while dragged.
T.SplitView {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    handle: Rectangle {
        implicitWidth: control.orientation === Qt.Horizontal ? 1 : control.width
        implicitHeight: control.orientation === Qt.Horizontal ? control.height : 1
        color: T.SplitHandle.pressed ? control.palette.highlight
               : T.SplitHandle.hovered ? Theme.border(control.palette)
               : Theme.hairline(control.palette)
        containmentMask: Item {
            x: control.orientation === Qt.Horizontal ? -3 : 0
            y: control.orientation === Qt.Horizontal ? 0 : -3
            width: control.orientation === Qt.Horizontal ? 7 : control.width
            height: control.orientation === Qt.Horizontal ? control.height : 7
        }
    }
}
