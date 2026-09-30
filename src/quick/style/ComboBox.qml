// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls
import QtQuick.Controls.impl

// A button-like field with a chevron; its list is a flat popup.
T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: padding + (!control.mirrored || !indicator || !indicator.visible ? 0 : indicator.width + spacing)
    rightPadding: padding + (control.mirrored || !indicator || !indicator.visible ? 0 : indicator.width + spacing)
    padding: 6
    spacing: 4

    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view.width
        text: model[control.textRole]
        highlighted: control.highlightedIndex === index
        font.weight: control.currentIndex === index ? Font.DemiBold : Font.Normal
        hoverEnabled: control.hoverEnabled
    }

    indicator: Chevron {
        x: control.mirrored ? control.padding : control.width - width - control.padding - 2
        y: control.topPadding + (control.availableHeight - height) / 2
        width: 10
        height: 10
        color: control.palette.windowText
        opacity: control.enabled ? 0.7 : 0.35
    }

    contentItem: T.TextField {
        leftPadding: 4
        rightPadding: 4
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        color: control.editable ? control.palette.text : control.palette.buttonText
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        verticalAlignment: Text.AlignVCenter
        opacity: control.enabled ? 1 : 0.45
    }

    background: Rectangle {
        implicitWidth: 140
        implicitHeight: Theme.controlHeight
        radius: Theme.radius
        color: control.editable ? control.palette.base
               : control.down ? Theme.pressed(control.palette)
               : control.hovered && control.enabled ? Theme.hovered(control.palette)
               : Theme.raised(control.palette)
        border.width: control.editable || control.visualFocus ? 1 : 0
        border.color: control.activeFocus ? control.palette.highlight : Theme.border(control.palette)
        opacity: control.enabled ? 1 : 0.6
        Behavior on color {
            ColorAnimation {
                duration: Theme.quick
            }
        }
    }

    popup: T.Popup {
        y: control.height + 2
        width: control.width
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding,
                         control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: ScrollIndicator {}
        }

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

        background: Rectangle {
            color: control.palette.base
            radius: Theme.popupRadius
            border.width: 1
            border.color: Theme.hairline(control.palette)
        }
    }
}
