// SPDX-License-Identifier: GPL-3.0-only
#include "bench/widget_color_scheme.hpp"

#include "bench/trackknife_style.hpp"
#include "uicommon/application_style.hpp"
#include "workspace/color_scheme.hpp"

#include <QApplication>

namespace trackknife::bench {

void followColorSchemes() {
    // ADR-0250: Trackknife's own style always; the scheme chosen decides
    // the colours, the desktop's included.
    QApplication::setStyle(new TrackknifeStyle);
    // A widget's own proxy over the style -- a list header's, a list's --
    // wraps another of these, not the desktop's default.
    ui::setApplicationStyleFactory([] { return new TrackknifeStyle; });
    ColorSchemes::instance().applyChosen();
}

} // namespace trackknife::bench
