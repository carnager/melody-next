// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>

class QStyle;

namespace trackknife::ui {

// A fresh instance of the style the application draws with, for a widget's
// own proxy over it (a header's, a list's): the proxy takes it, so it cannot
// be the application's own. The application's style has no factory name
// (ADR-0250), so it is made by the function registered here; without one,
// the factory's style of the same name, or Fusion.
[[nodiscard]] QStyle* createApplicationStyle();
void setApplicationStyleFactory(std::function<QStyle*()> factory);

} // namespace trackknife::ui
