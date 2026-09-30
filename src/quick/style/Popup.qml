// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

T.Popup {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: Theme.gapLarge

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

    background: Rectangle {
        radius: Theme.popupRadius
        color: control.palette.window
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }
    T.Overlay.modal: Rectangle {
        color: Theme.alpha(control.palette.shadow, 0.35)
    }
    T.Overlay.modeless: Rectangle {
        color: "transparent"
    }
}
