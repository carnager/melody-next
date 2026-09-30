// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

T.ProgressBar {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    contentItem: Item {
        implicitWidth: 120
        implicitHeight: 4
        Rectangle {
            width: control.indeterminate ? parent.width * 0.25 : control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.palette.highlight
            x: control.indeterminate ? indeterminateX : 0
            property real indeterminateX: 0
            NumberAnimation on indeterminateX {
                running: control.indeterminate && control.visible
                from: 0
                to: control.width * 0.75
                duration: 900
                loops: Animation.Infinite
            }
        }
    }
    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 4
        y: (control.height - height) / 2
        height: 4
        radius: 2
        color: Theme.hovered(control.palette)
    }
}
