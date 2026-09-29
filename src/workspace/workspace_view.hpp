// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/workspace.hpp"

#include <QString>

namespace trackknife::bench {

// What the workspace asks of the window drawing it (ADR-0220): to say
// something, and -- while the window's logic moves into the workspace --
// the parts of it that have not moved yet. Both windows implement it.
class WorkspaceView {
  public:
    WorkspaceView() = default;
    WorkspaceView(const WorkspaceView&) = delete;
    WorkspaceView& operator=(const WorkspaceView&) = delete;
    virtual ~WorkspaceView() = default;

    // A passing message, gone after `timeout_ms`.
    virtual void showMessage(const QString& text, int timeout_ms) = 0;
    // A list was edited: saved, and told to its engine when it plays.
    virtual void markTabDirty(Workspace::ListTab& tab) = 0;
    virtual void playRow(Workspace::ListTab& tab, int row) = 0;
    // An album's cover arrived: whatever shows it outside the lists -- the
    // player's header -- may now.
    virtual void artworkLoaded(const QString& key) = 0;
};

} // namespace trackknife::bench
