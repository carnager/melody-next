// SPDX-License-Identifier: GPL-3.0-only
import QtQuick

// Placeholder artwork: a colour derived from the album plus its initials.
Rectangle {
    id: cover

    property var album
    readonly property real hue: album ? album.hue : 0

    radius: 3
    border.color: Qt.rgba(1, 1, 1, 0.08)
    gradient: Gradient {
        GradientStop { position: 0; color: Qt.hsla(cover.hue, 0.45, 0.46, 1) }
        GradientStop { position: 1; color: Qt.hsla((cover.hue + 0.1) % 1, 0.55, 0.22, 1) }
    }

    Text {
        anchors.centerIn: parent
        visible: cover.height >= 24
        text: cover.album ? cover.album.title.split(/\s+/).filter(w => /\w/.test(w)).slice(0, 2)
                                .map(w => w.replace(/[^\w]/g, "")[0]).join("").toUpperCase() : ""
        color: Qt.rgba(1, 1, 1, 0.8)
        font.pixelSize: Math.round(cover.height * 0.34)
        font.weight: Font.DemiBold
    }
}
