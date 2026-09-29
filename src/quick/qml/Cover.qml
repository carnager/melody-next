// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// A cover from the engine, over a placeholder coloured from the album's
// name, which shows while it loads and when there is none.
Rectangle {
    id: cover

    property string source
    property string name

    readonly property real hue: {
        let hash = 0;
        for (let i = 0; i < name.length; ++i)
            hash = (hash * 31 + name.charCodeAt(i)) >>> 0;
        return (hash % 360) / 360;
    }

    radius: 3
    clip: true
    border.color: Qt.rgba(1, 1, 1, 0.08)
    gradient: Gradient {
        GradientStop { position: 0; color: Qt.hsla(cover.hue, 0.35, 0.40, 1) }
        GradientStop { position: 1; color: Qt.hsla((cover.hue + 0.1) % 1, 0.45, 0.20, 1) }
    }

    Text {
        anchors.centerIn: parent
        visible: image.status !== Image.Ready && cover.height >= 24
        text: cover.name.split(/\s+/).filter(w => /\w/.test(w)).slice(0, 2)
                  .map(w => w.replace(/[^\w]/g, "")[0] || "").join("").toUpperCase()
        color: Qt.rgba(1, 1, 1, 0.7)
        font.pixelSize: Math.round(cover.height * 0.34)
        font.weight: Font.DemiBold
    }

    Image {
        id: image
        anchors.fill: parent
        source: cover.source
        sourceSize.width: cover.width
        sourceSize.height: cover.height
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectCrop
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }
}
