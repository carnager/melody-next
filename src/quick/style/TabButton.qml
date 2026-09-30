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
        Behavior on color {
            ColorAnimation {
                duration: Theme.quick
            }
        }
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

    }
}
