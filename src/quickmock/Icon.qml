// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Shapes

// Small vector icons drawn on a 24x24 grid: `f` is filled, `s` is stroked.
Item {
    id: icon

    property string name
    property color color: Theme.text
    property real strokeWidth: 2

    implicitWidth: 16
    implicitHeight: 16

    readonly property var shapes: ({
        play: { f: "M8 5L19 12L8 19Z" },
        pause: { f: "M7 5h3.5v14H7z M13.5 5H17v14h-3.5z" },
        previous: { f: "M5 5h2.5v14H5z M20 5L9 12L20 19Z" },
        next: { f: "M16.5 5H19v14h-2.5z M4 5L15 12L4 19Z" },
        close: { s: "M7 7L17 17 M17 7L7 17" },
        chevron: { s: "M9.5 6L15.5 12L9.5 18" },
        menu: { s: "M4 7h16 M4 12h16 M4 17h16" },
        plus: { s: "M12 5v14 M5 12h14" },
        search: { s: "M10.5 4.5a6 6 0 1 0 0.01 0Z M15 15L20 20" },
        volume: { f: "M3 9h4l5-4v14l-5-4H3z", s: "M15.5 9a4 4 0 0 1 0 6 M18 6.5a7.5 7.5 0 0 1 0 11" },
        repeat: { s: "M4 11V9a2 2 0 0 1 2-2h13 M16 4l3 3l-3 3 M20 13v2a2 2 0 0 1-2 2H5 M8 20l-3-3l3-3" },
        shuffle: { s: "M3 7h3.5l10 10H21 M3 17h3.5l3-3 M13.5 10l3-3H21 M18 4l3 3l-3 3 M18 14l3 3l-3 3" },
        single: { s: "M12 3.5a8.5 8.5 0 1 0 0.01 0Z M10 9.5l2.2-1.5v8" },
        consume: { f: "M12 12L20.5 7.5A9.5 9.5 0 1 0 20.5 16.5Z" },
        queue: { s: "M4 6h13 M4 11h13 M4 16h8 M15.5 14v6l4.5-3z" },
        folder: { s: "M3.5 6.5a1 1 0 0 1 1-1h4.5l2 2h8.5a1 1 0 0 1 1 1v9.5a1 1 0 0 1-1 1h-15a1 1 0 0 1-1-1z" },
        history: { s: "M4 12a8 8 0 1 0 2.5-5.8 M4 4v4h4 M12 8v4.5l3 2" }
    })
    readonly property var shape: shapes[name] || ({})

    Shape {
        width: 24
        height: 24
        scale: icon.width / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            fillColor: icon.color
            strokeWidth: -1
            PathSvg { path: icon.shape.f || "" }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: icon.color
            strokeWidth: icon.strokeWidth
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.shape.s || "" }
        }
    }
}
