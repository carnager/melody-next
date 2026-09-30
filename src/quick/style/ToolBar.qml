// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

T.ToolBar {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: 4
    background: Rectangle {
        implicitHeight: 36
        color: control.palette.window
        Rectangle {
            width: parent.width
            height: 1
            y: control.position === T.ToolBar.Footer ? 0 : parent.height - 1
            color: Theme.hairline(control.palette)
        }
    }
}
