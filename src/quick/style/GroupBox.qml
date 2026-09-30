// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls

// No frame: a section with its title over a hairline.
T.GroupBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitLabelWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    spacing: Theme.gap
    padding: 0
    topPadding: (implicitLabelWidth > 0 ? implicitLabelHeight + spacing : 0) + Theme.gap

    label: Label {
        width: control.availableWidth
        text: control.title
        font.bold: true
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Item {
        Rectangle {
            y: control.implicitLabelHeight + 3
            width: parent.width
            height: 1
            color: Theme.hairline(control.palette)
            visible: control.title !== ""
        }
    }
}
