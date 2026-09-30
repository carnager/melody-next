// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// A folder, or (`file`) a page, drawn in one colour: the palette's, not an
// icon theme's.
Canvas {
    id: glyph
    property color color: "black"
    property bool file: false
    implicitWidth: 16
    implicitHeight: 16
    onColorChanged: requestPaint()
    onFileChanged: requestPaint()
    onPaint: {
        const context = getContext("2d");
        context.reset();
        context.strokeStyle = color;
        context.fillStyle = Qt.rgba(color.r, color.g, color.b, 0.18);
        context.lineWidth = 1.2;
        context.lineJoin = "round";
        context.beginPath();
        if (file) {
            context.moveTo(4, 2);
            context.lineTo(10, 2);
            context.lineTo(13, 5);
            context.lineTo(13, 14);
            context.lineTo(4, 14);
            context.closePath();
        } else {
            context.moveTo(1.5, 4);
            context.lineTo(6, 4);
            context.lineTo(7.5, 5.5);
            context.lineTo(14.5, 5.5);
            context.lineTo(14.5, 13);
            context.lineTo(1.5, 13);
            context.closePath();
        }
        context.fill();
        context.stroke();
    }
}
