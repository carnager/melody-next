// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A menu line: a column for its icon or its tick, then its text -- every
// text in a menu starting in the same place -- its key on the right, an
// arrow for a submenu; the selection tint when pointed at. As wide as its
// text needs.
T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 3
    leftPadding: 8
    rightPadding: 8
    spacing: 8
    icon.width: 16
    icon.height: 16
    // As compact as the desktop's own menus.
    font.pointSize: Theme.menuSize

    readonly property string keys: control.action && control.action.shortcut
                                   ? String(control.action.shortcut) : ""
    readonly property int column: 16 + control.spacing
    readonly property color ink: control.palette.text

    contentItem: Item {
        implicitWidth: control.column + label.implicitWidth
                       + (shortcut.visible ? shortcut.implicitWidth + 24 : 0)
                       + (control.subMenu ? 20 : 0)
        implicitHeight: Math.max(label.implicitHeight, 18)

        IconImage {
            anchors.verticalCenter: parent.verticalCenter
            width: control.icon.width
            height: control.icon.height
            sourceSize: Qt.size(control.icon.width, control.icon.height)
            source: control.icon.source
            name: control.icon.name
            color: control.icon.color
            visible: !control.checkable && (control.icon.source.toString() !== "" || control.icon.name !== "")
            opacity: control.enabled ? 1 : Theme.disabledOpacity
        }
        CheckMark {
            anchors.verticalCenter: parent.verticalCenter
            x: 3
            width: 10
            height: 10
            color: control.ink
            visible: control.checkable && control.checked
        }
        Text {
            id: label
            x: control.column
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - x - (shortcut.visible ? shortcut.implicitWidth + 24 : 0)
                   - (control.subMenu ? 20 : 0)
            text: control.text
            font: control.font
            color: control.ink
            elide: Text.ElideRight
            opacity: control.enabled ? 1 : Theme.disabledOpacity
        }
        Text {
            id: shortcut
            anchors.right: parent.right
            anchors.rightMargin: control.subMenu ? 20 : 0
            anchors.verticalCenter: parent.verticalCenter
            text: control.keys
            visible: text !== ""
            font.pointSize: Theme.smallSize
            color: Theme.dim(control.palette)
        }
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
        implicitWidth: 160
        implicitHeight: 24
        radius: Theme.radius
        color: (control.highlighted || control.down) && control.enabled
               ? Theme.selection(control.palette)
               : Theme.alpha(Theme.selection(control.palette), 0)
        Behavior on color {
            ColorAnimation {
                duration: 70
            }
        }
    }
}
