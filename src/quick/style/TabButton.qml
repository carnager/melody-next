// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

T.TabButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 6
    leftPadding: 12
    rightPadding: 12
    spacing: 6
    icon.width: 16
    icon.height: 16

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.checked ? control.palette.windowText : Theme.dim(control.palette)
    }

    background: Item {
        implicitHeight: 32
        Rectangle {
            anchors.fill: parent
            anchors.bottomMargin: 2
            radius: Theme.radius
            color: Theme.hovered(control.palette)
            visible: control.hovered && !control.checked
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            height: 2
            radius: 1
            color: control.palette.highlight
            visible: control.checked
        }
    }
}
