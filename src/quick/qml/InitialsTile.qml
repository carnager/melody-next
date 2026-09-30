// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

// What stands for a cover that is not there: a name's first letters on a
// tile coloured by that name -- the same name always the same colour, so an
// album is known by it at a glance. Quiet grey when there is no name.
Rectangle {
    id: tile

    property string name
    property real letterSize: Math.max(8, height * 0.38)

    readonly property string initials: {
        let letters = "";
        for (const word of tile.name.split(/\s+/)) {
            for (const character of word) {
                if (character.toUpperCase() !== character.toLowerCase()
                        || (character >= "0" && character <= "9")) {
                    letters += character.toUpperCase();
                    break;
                }
            }
            if (letters.length === 2)
                break;
        }
        return letters;
    }
    // A stable hue from the name: the same on every run and every machine.
    readonly property real hue: {
        let hash = 0;
        for (let i = 0; i < tile.name.length; ++i)
            hash = (hash * 31 + tile.name.charCodeAt(i)) >>> 0;
        return (hash % 360) / 360;
    }

    radius: 3
    color: initials === "" ? Shade.mix(palette.base, palette.text, 0.12)
                           : Qt.hsla(hue, 0.42, 0.40, 1)
    Label {
        anchors.centerIn: parent
        text: tile.initials
        font.pixelSize: tile.letterSize
        font.weight: Font.DemiBold
        color: tile.initials === "" ? tile.palette.placeholderText : "#f4f4f4"
    }
}
