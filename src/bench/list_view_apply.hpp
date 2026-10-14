// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/lists/list_view.hpp"
#include "uicommon/track_view_layout.hpp"

#include <memory>

class QTableView;

namespace trackknife::bench {

// ADR-0282: draws a list's table by a list view -- its columns, their order
// and what is shown, its grouping, artwork and rows -- at the widths
// `layout` has for them, and says the layout so applied. The table's model,
// a LocalListModel, is given the compiled view. The window draws its tabs by
// it, and the view editor its preview, saved or not.
[[nodiscard]] ui::TrackViewLayout
drawListView(QTableView* view, const lists::ListView& definition,
             const std::shared_ptr<const lists::CompiledListView>& compiled,
             const ui::TrackViewLayout& layout);

} // namespace trackknife::bench
