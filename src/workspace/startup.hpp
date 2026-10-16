// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace trackknife::bench {

// Before anything opens the settings or the workspace database, as either
// window starts.
//
// ADR-0287: the application is Melody. Its settings, its data and its cache
// move from Trackknife's names, once.
void adoptTrackknifeNames();
// A restore chosen last time (File › Restore workspace database…) is applied
// now, the database it replaces kept beside it. What happened, to tell the
// user; empty when nothing was pending.
[[nodiscard]] QString applyPendingWorkspaceRestore();

} // namespace trackknife::bench
