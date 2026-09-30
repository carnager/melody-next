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
}
