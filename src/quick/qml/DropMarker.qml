// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import Trackknife.Style

// Where a drop lands: a line in the accent between rows, or (`whole`) a
// tint over what takes it.
Rectangle {
    property bool whole: false
    visible: false
    z: 100
    radius: whole ? Theme.radius : 1
    color: whole ? Theme.alpha(palette.highlight, 0.18) : palette.highlight
    border.width: whole ? 1 : 0
    border.color: palette.highlight
    height: whole ? parent.height : 2
}
