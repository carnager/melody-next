// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: slider

    implicitHeight: 20
    padding: 0

    background: Item {
        x: slider.leftPadding
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: slider.availableWidth
        height: slider.hovered || slider.pressed ? 6 : 4
        Behavior on height { NumberAnimation { duration: 120 } }

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.line
        }
        Rectangle {
            width: slider.visualPosition * parent.width
            height: parent.height
            radius: height / 2
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: 12
        height: 12
        radius: 6
        color: Theme.text
        opacity: slider.hovered || slider.pressed ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
}
