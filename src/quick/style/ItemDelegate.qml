// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A row: a faint wash when hovered, the selection tint when chosen.
T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    leftPadding: 10
    rightPadding: 10
    spacing: 8
    icon.width: 16
    icon.height: 16

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: control.display === IconLabel.IconOnly || control.display === IconLabel.TextUnderIcon
                   ? Qt.AlignCenter : Qt.AlignLeft
        icon: control.icon
        text: control.text
        font: control.font
        color: control.palette.text
        opacity: control.enabled ? 1 : 0.45
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: Theme.rowHeight
        radius: Theme.radius
        color: control.highlighted || control.down ? Theme.selection(control.palette)
               : Theme.rowHover(control.palette)
        visible: control.highlighted || control.down || (control.hovered && control.enabled)
    }
}
