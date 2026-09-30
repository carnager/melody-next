// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>
#include <QString>

namespace trackknife::ui {

// The one star color used everywhere ratings render (ADR-0179).
[[nodiscard]] inline QColor ratingStarColor() { return QColor{245, 197, 24}; }

// A rating as a menu names it: "Unrate", "½ star", "1 star", "3½ stars".
[[nodiscard]] inline QString ratingMenuLabel(const unsigned rating) {
    if (rating == 0U) {
        return QStringLiteral("Unrate");
    }
    if (rating > 10U) {
        return {};
    }
    const auto half = rating % 2U != 0U;
    const auto stars = rating / 2U;
    if (half) {
        return stars == 0U ? QStringLiteral("½ star") : QStringLiteral("%1½ stars").arg(stars);
    }
    return stars == 1U ? QStringLiteral("1 star") : QStringLiteral("%1 stars").arg(stars);
}

} // namespace trackknife::ui
