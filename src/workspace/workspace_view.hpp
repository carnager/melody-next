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
// the parts of it that have not moved yet. The window implements it.
class WorkspaceView {
  public:
    WorkspaceView() = default;
    WorkspaceView(const WorkspaceView&) = delete;
    WorkspaceView& operator=(const WorkspaceView&) = delete;
    virtual ~WorkspaceView() = default;

    // The saved workspace is restored -- `restored`, or it could not be read.
    virtual void workspaceRestored(bool restored) = 0;
    // A passing message, gone after `timeout_ms`.
    virtual void showMessage(const QString& text, int timeout_ms) = 0;

    // The lists as the window shows them. A list added is drawn, and shown
    // at once when `select`; the list on show is the current one.
    virtual void listAdded(Workspace::ListTab& tab, bool select) = 0;
    virtual void showList(Workspace::ListTab& tab) = 0;
    [[nodiscard]] virtual Workspace::ListTab* currentList() = 0;
    [[nodiscard]] virtual std::vector<Workspace::ListTab*> listsInOrder() = 0;
    // The row a list's cursor is on, or -1.
    [[nodiscard]] virtual int currentRow(const Workspace::ListTab& tab) = 0;
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
    // The playback buffer profile chosen changed.
    virtual void refreshPlaybackBufferChecks() = 0;
    // Up Next changed.
    virtual void refreshUpNext() = 0;
    // The engines. One connected is shown -- its library among the sources
    // -- and one attached, now answering, is named as it says; one about to
    // be let go of is no longer shown, and when it is gone what was chosen
    // among them is chosen again. Settings' engines were connected again.
    virtual void engineConnected(Workspace::EngineLink& engine, bool first) = 0;
    virtual void engineAttached(Workspace::EngineLink& engine) = 0;
    // ADR-0234: it took the id it gave in place of `from`, and its lists
    // with it when `lists_follow`.
    virtual void engineRekeyed(Workspace::EngineLink& engine, const EngineKey& from,
                               bool lists_follow) = 0;
    virtual void engineRemoving(Workspace::EngineLink& engine) = 0;
    virtual void engineRemoved() = 0;
    virtual void enginesSynced() = 0;
    // An engine's lists changed, or who has which.
    virtual void engineListsChanged() = 0;
    // One engine's stored ratings changed.
    virtual void engineRatingsChanged(const EngineKey& engine,
                                      const QHash<QString, unsigned>& ratings) = 0;
    // File work the engines could not settle; `reported_now` when some is new.
    virtual void engineInterruptionsChanged(bool reported_now) = 0;
    // An album's cover arrived: whatever shows it outside the lists -- the
    // player's header -- may now.
    virtual void artworkLoaded(const QString& key) = 0;
};

} // namespace trackknife::bench
