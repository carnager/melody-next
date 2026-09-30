// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls

// A card over the window: its title, what it asks, its buttons on the right.
T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth,
                            implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: Theme.margin

    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.quick
            }
            NumberAnimation {
                property: "scale"
                from: 0.97
                to: 1
                duration: Theme.moderate
                easing.type: Easing.OutCubic
            }
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            to: 0
            duration: Theme.quick
        }
    }

    topPadding: 8

    background: Rectangle {
        radius: Theme.popupRadius
        color: control.palette.window
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }

    header: Label {
        text: control.title
        visible: control.title
        elide: Label.ElideRight
        font.bold: true
        padding: Theme.margin
        bottomPadding: 4
    }

    footer: DialogButtonBox {
        visible: count > 0
    }

    T.Overlay.modal: Rectangle {
        color: Theme.alpha(control.palette.shadow, 0.35)
    }
    T.Overlay.modeless: Rectangle {
        color: "transparent"
    }
}
