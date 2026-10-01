// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A field: the base colour, a hairline edge, the accent edge while typing.
T.TextField {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    padding: 6
    leftPadding: 8
    rightPadding: 8

    color: control.palette.text
    selectionColor: control.palette.highlight
    selectedTextColor: control.palette.highlightedText
    placeholderTextColor: Theme.dim(control.palette)
    verticalAlignment: TextInput.AlignVCenter
    opacity: enabled ? 1 : Theme.disabledOpacity

    PlaceholderText {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        visible: !control.length && !control.preeditText
                 && (!control.activeFocus || control.horizontalAlignment !== Qt.AlignHCenter)
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        implicitWidth: 160
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.readOnly ? Theme.sunken(control.palette) : control.palette.base
        border.width: 1
        border.color: control.activeFocus ? control.palette.highlight : Theme.border(control.palette)
        Behavior on border.color {
            ColorAnimation {
                duration: Theme.quick
            }
        }
    }
}
