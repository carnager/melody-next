// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls

T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - 4

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 6
    padding: 6
    leftPadding: 8
    rightPadding: 8
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
                 | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.toolTipText
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.palette.toolTipBase
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }
}
