// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QIcon>
#include <QStringView>

namespace trackknife::bench {

// An icon by its names: "edit-undo|sp:SP_ArrowBack" -- icon-theme names,
// then a style's standard pixmap (sp:), the first that exists. Where there is no icon theme (macOS, a bare desktop) a button so
// named still shows something.
[[nodiscard]] QIcon themedIcon(QStringView spec);

} // namespace trackknife::bench
