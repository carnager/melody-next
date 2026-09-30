// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls

T.ToolTip {
    id: control

    // By the pointer, below and to the right of it, as the desktop's:
    // placed as it shows, and kept inside the window by its margins.
    onAboutToShow: {
        const at = Pointer.at(parent);
        x = at.x + 2;
        y = at.y + 20;
    }

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 6
    padding: 6
    leftPadding: 8
    rightPadding: 8
    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.quick
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"
            to: 0
            duration: Theme.quick
        }
    }
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
                 | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.windowText
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.palette.base
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }
}
