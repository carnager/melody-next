// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Templates as T

T.Pane {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: Theme.gap
    background: Rectangle {
        color: control.palette.window
    }
}
