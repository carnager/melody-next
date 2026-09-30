// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

T.MenuBar {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    leftPadding: 4
    delegate: MenuBarItem {}
    contentItem: Row {
        spacing: control.spacing
        Repeater {
            model: control.contentModel
        }
    }
    background: Rectangle {
        implicitHeight: 28
        color: control.palette.window
    }
}
