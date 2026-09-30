// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A menu line: its text, its key on the right, a tick when it is on, an
// arrow for a submenu; the selection tint when pointed at.
T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    leftPadding: 8
    rightPadding: 8
    spacing: 8
    icon.width: 16
    icon.height: 16

    readonly property string keys: control.action && control.action.shortcut
                                   ? String(control.action.shortcut) : ""

    contentItem: Item {
        implicitWidth: label.implicitWidth + (control.keys !== "" ? shortcut.implicitWidth + 32 : 0)
                       + 20 + (control.subMenu ? 16 : 0)
        implicitHeight: label.implicitHeight
        IconLabel {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - anchors.leftMargin
                   - (control.keys !== "" ? shortcut.implicitWidth + 16 : 0)
            spacing: control.spacing
            mirrored: control.mirrored
            display: control.display
            alignment: Qt.AlignLeft
            icon: control.icon
            text: control.text
            font: control.font
            color: control.palette.text
            opacity: control.enabled ? 1 : 0.45
        }
        Text {
            id: shortcut
            anchors.right: parent.right
            anchors.rightMargin: control.subMenu ? 16 : 0
            anchors.verticalCenter: parent.verticalCenter
            text: control.keys
            visible: text !== ""
            font.pointSize: Theme.smallSize
            color: Theme.dim(control.palette)
        }
    }

    indicator: CheckMark {
        x: control.leftPadding + 2
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 10
        height: 10
        color: control.palette.text
        visible: control.checkable && control.checked
    }

    arrow: Chevron {
        x: control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 9
        height: 9
        rotation: -90
        color: control.palette.text
        visible: control.subMenu
        opacity: 0.7
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 26
        radius: Theme.radius
        color: Theme.selection(control.palette)
        visible: (control.highlighted || control.down) && control.enabled
    }
}
