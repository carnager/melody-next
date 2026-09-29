// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QColor>

namespace trackknife::ui {

// The one star color used everywhere ratings render (ADR-0179).
[[nodiscard]] inline QColor ratingStarColor() { return QColor{245, 197, 24}; }

} // namespace trackknife::ui
