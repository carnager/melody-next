// SPDX-License-Identifier: GPL-3.0-only
pragma Singleton
import QtQuick

// The colour mixes the widgets window derives from the palette -- Window and
// Text at a few percent, Highlight at 35 %, Base at alpha 110 -- so what is
// painted here matches what is painted there.
QtObject {
    // `a` moved `t` of the way toward `b`.
    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                       a.a + (b.a - a.a) * t);
    }
    function alpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a);
    }
    // QColor::lighter(factor): the HSV value scaled by factor / 100.
    function lighter(c, factor) {
        return Qt.lighter(c, factor / 100);
    }
    function luma(c) {
        return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    }
    // An accent as text on `ground`: lighter on a dark ground, darker on a
    // light one, and further until it reads -- a pale highlight on white, as
    // a light desktop gives an unfocused window, would otherwise vanish.
    function accentOn(accent, ground) {
        const light = luma(ground) > 0.5;
        let shade = light ? Qt.darker(accent, 1.15) : Qt.lighter(accent, 1.15);
        for (let step = 0; step < 6 && Math.abs(luma(shade) - luma(ground)) < 0.4; ++step)
            shade = light ? Qt.darker(shade, 1.3) : Qt.lighter(shade, 1.3);
        return shade;
    }
}
