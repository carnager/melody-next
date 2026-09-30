// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls

// Buttons at the right end; the accepting one in the accent.
T.DialogButtonBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            (control.count === 1 ? implicitContentWidth * 2 : implicitContentWidth)
                            + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    spacing: Theme.gap
    padding: Theme.margin
    topPadding: Theme.gap
    alignment: Qt.AlignRight

    delegate: Button {
        width: Math.min(implicitWidth, control.count === 1 ? control.availableWidth / 2
                                                           : control.availableWidth / control.count)
        highlighted: DialogButtonBox.buttonRole === DialogButtonBox.AcceptRole
                     || DialogButtonBox.buttonRole === DialogButtonBox.YesRole
    }

    contentItem: ListView {
        implicitWidth: contentWidth
        model: control.contentModel
        spacing: control.spacing
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
    }

    background: Item {
        implicitHeight: Theme.controlHeight
    }
}
