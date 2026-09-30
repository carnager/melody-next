// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

// The header's pills (the output device and Up Next): background at 6 %
// toward Text, a 1 px border at 16 %, radius 13, 10 px sides; the border
// takes Highlight on hover; pressed or checked fills at 16 %. Font 0.92x.
AbstractButton {
    id: pill

    property bool chevron: false
    readonly property color ground: pill.palette.window
    readonly property color ink: pill.palette.text

    implicitHeight: 26
    leftPadding: 10
    rightPadding: chevron ? 24 : 10
    font.pointSize: Qt.application.font.pointSize * 0.92
    hoverEnabled: true

    background: Rectangle {
        radius: 13
        color: pill.down || pill.checked ? Shade.mix(pill.ground, pill.ink, 0.16)
                                         : Shade.mix(pill.ground, pill.ink, 0.06)
        border.width: 1
        border.color: pill.hovered ? pill.palette.highlight : Shade.mix(pill.ground, pill.ink, 0.16)
    }
}
