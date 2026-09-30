// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls
import Trackknife.Quick

// A rating in a Rate menu: five stars, as many filled as it gives, and a
// check where the whole selection has it; "Unrate" is plain text.
MenuItem {
    id: item

    property int rating: 0
    property bool shared: false

    text: Tk.ratingLabel(rating)
    Accessible.name: text
    implicitHeight: rating === 0 ? implicitContentHeight + topPadding + bottomPadding : 26
    checkable: rating === 0
    checked: rating === 0 && shared

    contentItem: Item {
        implicitWidth: item.rating === 0 ? label.implicitWidth + 24 : 24 + 5 * 16 + 4 * 3 + 14
        implicitHeight: item.rating === 0 ? label.implicitHeight : 26
        Label {
            id: label
            visible: item.rating === 0
            anchors.verticalCenter: parent.verticalCenter
            text: item.text
            color: item.highlighted ? item.palette.highlightedText : item.palette.text
        }
        Canvas {
            id: stars
            visible: item.rating > 0
            anchors.fill: parent
            property bool lit: item.highlighted
            property bool mark: item.shared
            onLitChanged: requestPaint()
            onMarkChanged: requestPaint()
            onPaint: {
                const context = getContext("2d");
                context.reset();
                if (mark) {
                    // The check where the selection shares this rating.
                    context.strokeStyle = lit ? item.palette.highlightedText : item.palette.text;
                    context.lineWidth = 1.6;
                    const top = height / 2 - 6;
                    context.beginPath();
                    context.moveTo(6, top + 6);
                    context.lineTo(12, top + 12);
                    context.lineTo(18, top);
                    context.stroke();
                }
                const star = (cx, cy, radius) => {
                    context.beginPath();
                    for (let point = 0; point < 10; ++point) {
                        const r = point % 2 === 0 ? radius : radius * 0.42;
                        const angle = -Math.PI / 2 + point * Math.PI / 5;
                        const x = cx + Math.cos(angle) * r, y = cy + Math.sin(angle) * r;
                        if (point === 0)
                            context.moveTo(x, y);
                        else
                            context.lineTo(x, y);
                    }
                    context.closePath();
                };
                let left = 24;
                for (let index = 0; index < 5; ++index) {
                    star(left + 8, height / 2, 8);
                    if (index < item.rating / 2) {
                        context.fillStyle = "#f5c518";
                        context.fill();
                    } else {
                        context.strokeStyle = Qt.rgba(245 / 255, 197 / 255, 24 / 255, 130 / 255);
                        context.lineWidth = 1.2;
                        context.stroke();
                    }
                    left += 16 + 3;
                }
            }
        }
    }
}
