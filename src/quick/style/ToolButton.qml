// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// Nothing behind it until it is hovered, pressed or on.
T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 6
    leftPadding: 8
    rightPadding: 8
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
        color: control.checked ? control.palette.highlight : control.palette.windowText
        opacity: control.enabled ? 1 : Theme.disabledOpacity
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.down ? Theme.pressed(control.palette)
               : control.checked ? Theme.alpha(control.palette.highlight, 0.18)
               : control.hovered && control.enabled ? Theme.hovered(control.palette)
               : Theme.alpha(Theme.hovered(control.palette), 0)
        Behavior on color {
            ColorAnimation {
                duration: Theme.quick
            }
        }
    }
}
