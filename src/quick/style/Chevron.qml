// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// A small open arrow, pointing down (or `rotation` elsewhere).
Canvas {
    id: chevron
    property color color: "black"
    implicitWidth: 10
    implicitHeight: 10
    onColorChanged: requestPaint()
    onPaint: {
        const context = getContext("2d");
        context.reset();
        context.strokeStyle = color;
        context.lineWidth = 1.5;
        context.lineCap = "round";
        context.lineJoin = "round";
        context.beginPath();
        context.moveTo(width * 0.2, height * 0.35);
        context.lineTo(width * 0.5, height * 0.65);
        context.lineTo(width * 0.8, height * 0.35);
        context.stroke();
    }
}
