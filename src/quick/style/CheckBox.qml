// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl

// A rounded box that fills with the accent and a tick when on.
T.CheckBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: 4
    spacing: 8

    readonly property bool on: control.checkState !== Qt.Unchecked

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding
                                            : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 3
        color: control.on ? control.palette.highlight : control.palette.base
        border.width: control.on ? 0 : 1
        border.color: control.visualFocus || control.hovered ? control.palette.highlight
                                                             : Theme.border(control.palette)
        opacity: control.enabled ? 1 : 0.45
        CheckMark {
            anchors.centerIn: parent
            width: 10
            height: 10
            color: control.palette.highlightedText
            partial: control.checkState === Qt.PartiallyChecked
            visible: control.on
        }
    }

    contentItem: CheckLabel {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.palette.windowText
        opacity: control.enabled ? 1 : 0.45
    }
}
