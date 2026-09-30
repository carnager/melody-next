// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

T.MenuBarItem {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 4
    leftPadding: 10
    font.pointSize: Theme.menuSize
    rightPadding: 10
    topInset: 3
    bottomInset: 3
    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: Qt.AlignLeft
        icon: control.icon
        text: control.text
        font: control.font
        color: control.palette.windowText
    }
    background: Rectangle {
        implicitWidth: 20
        implicitHeight: 22
        radius: Theme.radius
        color: control.down || control.highlighted ? Theme.hovered(control.palette) : "transparent"
    }
}
