// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

// The Trackknife style's measures and colours: one set of spacings and
// radii for every window, and the few colours it paints, each mixed from the
// palette of the control it is for -- so a light desktop and a dark one both
// get the flat, quiet look, in their own colours.
QtObject {
    // Spacing: between related items, between groups, around a window.
    readonly property int gapSmall: 4
    readonly property int gap: 8
    readonly property int gapLarge: 16
    readonly property int margin: 16
    // A form's labels line up in a column this wide.
    readonly property int labelWidth: 160
    readonly property int radius: 4
    readonly property int popupRadius: 6
    readonly property int controlHeight: 28
    readonly property int rowHeight: 28
    readonly property real smallSize: Qt.application.font.pointSize * 0.9
    // Menus read smaller than the window, as the desktop's own do.
    readonly property real menuSize: Qt.application.font.pointSize * 0.95

    // `a` moved `t` of the way toward `b`.
    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                       a.a + (b.a - a.a) * t);
    }
    function alpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a);
    }

    // Lines between things, and around what is typed into.
    function hairline(p) {
        return mix(p.window, p.windowText, 0.13);
    }
    function border(p) {
        return mix(p.window, p.windowText, 0.22);
    }
    // A button at rest, hovered, pressed.
    function raised(p) {
        return mix(p.window, p.windowText, 0.07);
    }
    function hovered(p) {
        return mix(p.window, p.windowText, 0.11);
    }
    function pressed(p) {
        return mix(p.window, p.windowText, 0.17);
    }
    // A side bar, a header: a shade off the window.
    function sunken(p) {
        return mix(p.window, p.windowText, 0.035);
    }
    // Rows: hovered, and chosen -- the widgets window's highlight at 35 %.
    function rowHover(p) {
        return mix(p.base, p.text, 0.05);
    }
    function selection(p) {
        return mix(p.base, p.highlight, 0.35);
    }
    // Text that says less: hints, counts, notes.
    function dim(p) {
        return mix(p.windowText, p.window, 0.45);
    }
}
