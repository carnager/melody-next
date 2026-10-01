// SPDX-License-Identifier: GPL-3.0-only
#include "bench/widget_color_scheme.hpp"

#include "bench/trackknife_style.hpp"
#include "workspace/color_scheme.hpp"

#include <QApplication>

namespace trackknife::bench {

void followColorSchemes() {
    // ADR-0250: Trackknife's own style always, as the Qt Quick window draws
    // its own; the scheme chosen decides the colours, the desktop's included.
    QApplication::setStyle(new TrackknifeStyle);
    ColorSchemes::instance().applyChosen();
}

} // namespace trackknife::bench
