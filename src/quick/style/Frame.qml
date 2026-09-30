// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

// A surface for lists: the base colour inside a hairline.
T.Frame {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: 1
    background: Rectangle {
        radius: Theme.radius
        color: control.palette.base
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }
}
