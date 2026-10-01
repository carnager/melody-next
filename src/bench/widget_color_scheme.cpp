// SPDX-License-Identifier: GPL-3.0-only
#include "bench/widget_color_scheme.hpp"

#include "workspace/color_scheme.hpp"

#include <QApplication>
#include <QStyle>

namespace trackknife::bench {

void followColorSchemes() {
    auto& schemes = ColorSchemes::instance();
    const auto desktop_style = QApplication::style()->name();
    QObject::connect(&schemes, &ColorSchemes::applied, qApp, [&schemes, desktop_style] {
        const auto wanted = schemes.ownPalette() ? QStringLiteral("fusion") : desktop_style;
        if (QApplication::style()->name().compare(wanted, Qt::CaseInsensitive) != 0) {
            QApplication::setStyle(wanted);
        }
        // A style brings its own palette: the scheme's goes over it again.
        if (schemes.ownPalette()) {
            QApplication::setPalette(schemes.scheme() == ColorScheme::light ? lightPalette()
                                                                            : darkPalette());
        }
    });
    schemes.applyChosen();
}

} // namespace trackknife::bench
