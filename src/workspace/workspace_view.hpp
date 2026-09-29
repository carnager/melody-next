// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/workspace.hpp"

#include "uicommon/track_view_layout.hpp"

#include <QObject>
#include <QString>

#include <vector>

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

    // The lists as the window shows them. A list added is drawn, and shown
    // at once when `select`; the list on show is the current one.
    virtual void listAdded(Workspace::ListTab& tab, bool select) = 0;
    virtual void showList(Workspace::ListTab& tab) = 0;
    [[nodiscard]] virtual Workspace::ListTab* currentList() = 0;
    [[nodiscard]] virtual std::vector<Workspace::ListTab*> listsInOrder() = 0;
    // How a list's columns are arranged now, to be saved with it.
    [[nodiscard]] virtual ui::TrackViewLayout
    captureTrackViewLayout(const Workspace::ListTab& tab) const = 0;
    // Who owns a list's model: it has to outlive what shows it.
    [[nodiscard]] virtual QObject* modelParent() = 0;
    // A list's name, kind or state changed.
    virtual void refreshTabChrome(Workspace::ListTab& tab) = 0;
    // The list edits that can be undone or redone changed.
    virtual void refreshListHistoryActions() = 0;
    // What is selected changed, or what it is.
    virtual void refreshSelectionStatus() = 0;
    // What plays, or how, changed: the transport, the cursor on the playing
    // row (brought into view when `jump`), and the mode controls.
    virtual void refreshTransport() = 0;
    virtual void refreshPlaybackCursor(bool jump) = 0;
    virtual void refreshLocalPlaybackControls() = 0;
    // Hands the engines the file moves they have not taken yet.
    virtual void flushEngineRelocations() = 0;
    // An album's cover arrived: whatever shows it outside the lists -- the
    // player's header -- may now.
    virtual void artworkLoaded(const QString& key) = 0;
};

} // namespace trackknife::bench
