// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// A tick, or a dash for a partial check.
Canvas {
    id: mark
    property color color: "white"
    property bool partial: false
    implicitWidth: 10
    implicitHeight: 10
    onColorChanged: requestPaint()
    onPartialChanged: requestPaint()
    onPaint: {
        const context = getContext("2d");
        context.reset();
        context.strokeStyle = color;
        context.lineWidth = 1.8;
        context.lineCap = "round";
        context.lineJoin = "round";
        context.beginPath();
        if (partial) {
            context.moveTo(width * 0.2, height * 0.5);
            context.lineTo(width * 0.8, height * 0.5);
        } else {
            context.moveTo(width * 0.15, height * 0.52);
            context.lineTo(width * 0.4, height * 0.78);
            context.lineTo(width * 0.85, height * 0.25);
        }
        context.stroke();
    }
}
