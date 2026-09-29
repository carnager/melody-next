// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

namespace trackknife::bench {

// Before anything opens the settings or the workspace database, as either
// window starts.
//
// The application reclaimed its original name: settings and the database
// written under the interim "trackbench" identity are adopted, once.
void adoptInterimIdentity();
// A restore chosen last time (File › Restore workspace database…) is applied
// now, the database it replaces kept beside it. What happened, to tell the
// user; empty when nothing was pending.
[[nodiscard]] QString applyPendingWorkspaceRestore();

} // namespace trackknife::bench
