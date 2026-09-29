// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// A 0-10 rating as five stars, in halves. Pointing previews, clicking sets;
// clicking the rating it already has clears it.
Row {
    id: stars

    property int rating: 0
    property int preview: -1
    readonly property int shown: preview >= 0 ? preview : rating
    signal rated(int value)

    spacing: 1

    Repeater {
        model: 5
        delegate: Item {
            id: star
            required property int index
            width: 13
            height: 16

            Text {
                anchors.centerIn: parent
                text: "★"
                color: Theme.line
                font.pixelSize: 13
            }
            Item {
                clip: true
                height: parent.height
                width: stars.shown >= (star.index + 1) * 2 ? parent.width
                     : stars.shown === star.index * 2 + 1 ? parent.width / 2 : 0
                Text {
                    x: (star.width - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    text: "★"
                    color: stars.preview >= 0 ? Theme.accent : Theme.star
                    font.pixelSize: 13
                }
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onPositionChanged: mouse => stars.preview = star.index * 2 + (mouse.x < width / 2 ? 1 : 2)
                onExited: stars.preview = -1
                onClicked: {
                    const value = stars.preview >= 0 ? stars.preview : (star.index + 1) * 2;
                    stars.rated(value === stars.rating ? 0 : value);
                }
            }
        }
    }
}
