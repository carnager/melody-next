// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// Flat: a quiet fill that deepens when hovered and pressed; the accent for
// the default (highlighted) button, as a dialog's Save is.
T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 6
    leftPadding: 14
    rightPadding: 14
    spacing: 6
    icon.width: 16
    icon.height: 16

    readonly property bool accent: control.highlighted || control.checked

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.accent ? control.palette.highlightedText : control.palette.buttonText
        opacity: control.enabled ? 1 : Theme.disabledOpacity
    }

    background: Rectangle {
        implicitWidth: 72
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.flat && !control.down && !control.hovered && !control.accent
               ? Theme.alpha(Theme.hovered(control.palette), 0)
               : control.accent
               ? (control.down ? Qt.darker(control.palette.highlight, 1.15)
                  : control.hovered ? Qt.lighter(control.palette.highlight, 1.08)
                  : control.palette.highlight)
               : control.down ? Theme.pressed(control.palette)
               : control.hovered && control.enabled ? Theme.hovered(control.palette)
               : Theme.raised(control.palette)
        border.width: control.visualFocus ? 1 : 0
        border.color: control.palette.highlight
        Behavior on color {
            ColorAnimation {
                duration: Theme.quick
            }
        }
    }
}
