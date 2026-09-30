// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls
import QtQuick.Window

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 0
    padding: 4
    overlap: 2

    delegate: MenuItem {}

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


    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window ? contentHeight + control.topPadding + control.bottomPadding
                                     > control.height : false
        clip: true
        currentIndex: control.currentIndex
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 24
        radius: Theme.popupRadius
        color: control.palette.base
        border.width: 1
        border.color: Theme.hairline(control.palette)
    }
}
