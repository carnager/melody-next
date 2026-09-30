// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

// ui::LineSlider: a flat 4 px bar with no handle. The track is Text at alpha
// 0.28 (0.16 disabled), the part done Highlight (alpha 0.35 disabled); a
// click jumps to where it was made. `moved` while pressed, `released` when
// let go -- the seek bar seeks on release only.
Slider {
    id: slider

    implicitHeight: 18
    padding: 0
    live: true

    background: Item {
        x: slider.leftPadding
        width: slider.availableWidth
        height: slider.height
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Shade.alpha(slider.palette.text, slider.enabled ? 0.28 : 0.16)
            Rectangle {
                width: slider.visualPosition * parent.width
                height: parent.height
                radius: 2
                color: Shade.alpha(slider.palette.highlight, slider.enabled ? 1.0 : 0.35)
            }
        }
    }
    handle: Item {}
}
