// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A field with two small steppers at its end.
T.SpinBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 4
    leftPadding: 8
    rightPadding: 8 + up.indicator.width

    validator: IntValidator {
        locale: control.locale.name
        bottom: Math.min(control.from, control.to)
        top: Math.max(control.from, control.to)
    }

    contentItem: TextInput {
        z: 2
        text: control.displayText
        font: control.font
        color: control.palette.text
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        clip: width < implicitWidth
        opacity: control.enabled ? 1 : 0.45
    }

    up.indicator: Item {
        x: control.mirrored ? 1 : control.width - width - 1
        y: 1
        implicitWidth: 20
        height: control.height / 2 - 1
        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: control.up.pressed ? Theme.pressed(control.palette) : Theme.hovered(control.palette)
            visible: control.up.pressed || control.up.hovered
        }
        Chevron {
            anchors.centerIn: parent
            width: 8
            height: 8
            rotation: 180
            color: control.palette.text
            opacity: control.up.indicator.enabled ? 0.7 : 0.3
        }
    }

    down.indicator: Item {
        x: control.mirrored ? 1 : control.width - width - 1
        y: control.height / 2
        implicitWidth: 20
        height: control.height / 2 - 1
        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: control.down.pressed ? Theme.pressed(control.palette) : Theme.hovered(control.palette)
            visible: control.down.pressed || control.down.hovered
        }
        Chevron {
            anchors.centerIn: parent
            width: 8
            height: 8
            color: control.palette.text
            opacity: control.down.indicator.enabled ? 0.7 : 0.3
        }
    }

    background: Rectangle {
        implicitWidth: 110
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.palette.base
        border.width: 1
        border.color: control.activeFocus ? control.palette.highlight : Theme.border(control.palette)
        opacity: control.enabled ? 1 : 0.6
    }
}
