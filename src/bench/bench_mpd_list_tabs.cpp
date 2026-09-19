// SPDX-License-Identifier: GPL-3.0-only

// ADR-0191: every list lives on the server. A working tab is a scratch
// list — an ordinary MPD stored playlist flagged so it appears in the tab
// strip instead of beside curated playlists. This file holds the tab-strip
// grouping, the send-to-tab gestures, and scratch-list creation.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"

#include "quick/mpd_probe_controller.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "quick/mpd_queue_model.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QHeaderView>
#include <QScrollArea>
#include <QFrame>
#include <QIcon>
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <chrono>
#include <QAction>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTableView>

#include <algorithm>
#include <utility>

namespace trackknife::bench {



// mpdTabInsertionIndex keeps every server-side tab grouped directly after
// the MPD Queue: new MPD tabs insert after the last existing one instead of
// trailing the local lists.
int BenchMainWindow::mpdTabInsertionIndex() {
    auto index = mpd_queue_view_ != nullptr ? tabs_->indexOf(mpd_queue_view_) : -1;
    if (index < 0) {
        return tabs_->count();
    }
    ++index;
    while (index < tabs_->count()) {
        auto* widget = tabs_->widget(index);
        if (mpdPlaylistTabForWidget(qobject_cast<QTableView*>(widget)) == nullptr &&
            mpdSearchTabForWidget(qobject_cast<QTableView*>(widget)) == nullptr) {
            break;
        }
        ++index;
    }
    return index;
}
std::vector<mpd::Track> BenchMainWindow::selectedMpdViewTracks(QTableView* view) const {
    std::vector<mpd::Track> tracks;
    if (view == nullptr || view->selectionModel() == nullptr) {
        return tracks;
    }
    const auto* model = qobject_cast<const quick::MpdQueueModel*>(view->model());
    if (model == nullptr) {
        return tracks;
    }
    auto selected = view->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    tracks.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& index : selected) {
        if (const auto* track = model->trackAt(index.row())) {
            tracks.push_back(*track);
        }
    }
    return tracks;
}

// A fresh working-list name that no playlist is using — the drop gesture
// has no dialog to ask in, so it names the list itself.
QString BenchMainWindow::uniqueScratchListName() {
    const auto taken = mpdPlaylistNames();
    for (int suffix = 1; suffix < 1'000; ++suffix) {
        const auto candidate = QStringLiteral("Working list %1").arg(suffix);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
    return QStringLiteral("Working list");
}

// ADR-0191: a working tab is created on the server — a stored playlist
// flagged scratch, so it belongs to whoever connects rather than to this
// client, and opens as a tab instead of a sidebar entry.
void BenchMainWindow::createScratchListTab(const QString& name, const QStringList& uris) {
    if (name.isEmpty() || uris.isEmpty()) {
        return;
    }
    // The list is the server's, so there is nothing to create without one.
    if (!mpd_controller_->connected() ||
        !mpd_controller_->supportsCommand(QStringLiteral("playlistadd"))) {
        statusBar()->showMessage(QStringLiteral("Connect to MPD to create a working list"), 3'000);
        return;
    }
    mpd_controller_->addToStoredPlaylist(name, uris, -1);
    mpd_controller_->setPlaylistScratch(name, true);
    if (auto* tab = openMpdPlaylistTab(name, true)) {
        tab->scratch = true;
    }
    refreshMpdPlaylistsSoon();
}

// Promotion clears the flag: the list stops being a working tab and takes
// its place among the curated playlists, contents untouched.
void BenchMainWindow::promoteScratchList(const QString& name) {
    mpd_controller_->setPlaylistScratch(name, false);
    if (auto* tab = mpdPlaylistTabNamed(name)) {
        tab->scratch = false;
        refreshMpdPlaylistTabChrome(*tab);
    }
    refreshMpdPlaylistsSoon();
    statusBar()->showMessage(QStringLiteral("%1 is a playlist now").arg(name), 4'000);
}

// Closing a working list deletes it, so the gesture asks — with "Keep it"
// offering promotion instead, which is how a scratch list becomes a real
// playlist without copying anything.
void BenchMainWindow::confirmCloseScratchList(const QString& name) {
    QMessageBox prompt{this};
    prompt.setWindowTitle(QStringLiteral("Close working list"));
    prompt.setText(QStringLiteral("Close “%1”?").arg(name));
    prompt.setInformativeText(
        QStringLiteral("Working lists live on the server only while their tab is open."));
    auto* discard = prompt.addButton(QStringLiteral("Delete list"), QMessageBox::DestructiveRole);
    auto* keep = prompt.addButton(QStringLiteral("Keep as playlist"), QMessageBox::AcceptRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(keep);
    prompt.exec();
    const auto* clicked = prompt.clickedButton();
    if (clicked == keep) {
        promoteScratchList(name);
        closeMpdPlaylistTab(name);
        return;
    }
    if (clicked == discard) {
        mpd_controller_->deleteStoredPlaylist(name);
        closeMpdPlaylistTab(name);
        refreshMpdPlaylistsSoon();
    }
}

// Adds the selection to a curated stored playlist, or starts one. Scratch
// lists are offered the same way through "Send to tab".
void BenchMainWindow::addCopyToServerListMenu(QMenu* menu, QTableView* source_view) {
    const auto uris = selectedMpdViewUris(source_view);
    auto* submenu = menu->addMenu(QStringLiteral("Add to playlist"));
    submenu->setObjectName(QStringLiteral("bench-mpd-add-to-playlist-menu"));
    const auto ready = !uris.isEmpty() && mpd_controller_->connected() &&
                       mpd_controller_->supportsCommand(QStringLiteral("playlistadd"));
    submenu->setEnabled(ready);
    auto* create = submenu->addAction(QStringLiteral("New playlist…"));
    create->setObjectName(QStringLiteral("action-copy-to-new-server-list"));
    create->setEnabled(ready);
    connect(create, &QAction::triggered, this, [this, uris] {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("New playlist"),
                                                QStringLiteral("Playlist name:"),
                                                QLineEdit::Normal,
                                                QStringLiteral("Playlist"), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        mpd_controller_->addToStoredPlaylist(name, uris, -1);
        openMpdPlaylistTab(name, true);
    });
    const auto playlist_names = mpdPlaylistNames();
    if (!playlist_names.isEmpty()) {
        submenu->addSeparator();
        for (const auto& name : playlist_names) {
            auto* action = submenu->addAction(name);
            action->setEnabled(ready);
            connect(action, &QAction::triggered, this,
                    [this, name, uris] { mpd_controller_->addToStoredPlaylist(name, uris, -1); });
        }
    }
}
// ADR-0190: the tabs a selection of server tracks can be sent to. The queue
// tab is always one of them; working tabs and open stored playlist tabs join
// it, so "send this somewhere" needs no special case per surface.
std::vector<BenchMainWindow::MpdTabTarget> BenchMainWindow::mpdTabTargets() const {
    std::vector<MpdTabTarget> targets;
    targets.push_back(MpdTabTarget{.kind = MpdTabTarget::Kind::queue,
                                   .label = QStringLiteral("MPD Queue"),
                                   .playlist = {}});
    for (const auto& tab : mpd_playlist_tabs_) {
        targets.push_back(MpdTabTarget{.kind = MpdTabTarget::Kind::playlist,
                                       .label = tab->name,
                                       .playlist = tab->name});
    }
    return targets;
}

// The tab the user is looking at, when it can hold server tracks. A local
// list tab cannot (ADR-0058), so there the queue stays the default target.
std::optional<BenchMainWindow::MpdTabTarget> BenchMainWindow::visibleMpdTabTarget() const {
    auto* current = tabs_ == nullptr ? nullptr : tabs_->currentWidget();
    if (current == nullptr) {
        return std::nullopt;
    }
    if (current == mpd_queue_view_) {
        return MpdTabTarget{.kind = MpdTabTarget::Kind::queue,
                            .label = QStringLiteral("MPD Queue"),
                            .playlist = {}};
    }
    for (const auto& tab : mpd_playlist_tabs_) {
        if (tab->view == current) {
            return MpdTabTarget{.kind = MpdTabTarget::Kind::playlist,
                                .label = tab->name,
                                .playlist = tab->name};
        }
    }
    return std::nullopt;
}

void BenchMainWindow::sendTracksToMpdTab(const MpdTabTarget& target,
                                         std::vector<mpd::Track> tracks, const MpdSendMode mode) {
    if (tracks.empty()) {
        return;
    }
    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    switch (target.kind) {
    case MpdTabTarget::Kind::queue:
        // The controller routes these to the queue context, which is the
        // server's stash while another list is the active queue.
        switch (mode) {
        case MpdSendMode::append:
            mpd_controller_->addUris(uris, false);
            break;
        case MpdSendMode::insert_next:
            mpd_controller_->addUris(uris, true);
            break;
        case MpdSendMode::replace:
            mpd_controller_->replaceQueueWithUris(uris);
            break;
        }
        return;
    case MpdTabTarget::Kind::playlist:
        switch (mode) {
        case MpdSendMode::append:
            mpd_controller_->addToStoredPlaylist(target.playlist, uris, -1);
            break;
        case MpdSendMode::insert_next: {
            auto* tab = mpdPlaylistTabNamed(target.playlist);
            const auto row = tab != nullptr && tab->view != nullptr &&
                                     tab->view->currentIndex().isValid()
                                 ? tab->view->currentIndex().row() + 1
                                 : 0;
            mpd_controller_->addToStoredPlaylist(target.playlist, uris, row);
            break;
        }
        case MpdSendMode::replace:
            mpd_controller_->clearStoredPlaylist(target.playlist);
            mpd_controller_->addToStoredPlaylist(target.playlist, uris, -1);
            break;
        }
        return;
    }
}

// "Send to" lists every tab, each with the three placements. The two direct
// actions above it already cover the common case — the visible tab — so this
// is for aiming somewhere else without switching tabs first.
void BenchMainWindow::addSendToTabMenu(QMenu* menu,
                                       const std::function<std::vector<mpd::Track>()>& selection) {
    auto* submenu = menu->addMenu(QStringLiteral("Send to tab"));
    submenu->setObjectName(QStringLiteral("bench-send-to-tab-menu"));
    const auto targets = mpdTabTargets();
    const auto connected = mpd_controller_->connected();
    auto* create = submenu->addAction(QStringLiteral("New list…"));
    create->setObjectName(QStringLiteral("action-send-to-new-working-list"));
    create->setEnabled(connected && mpd_controller_->supportsCommand(
                                        QStringLiteral("playlistadd")));
    connect(create, &QAction::triggered, this, [this, selection] {
        const auto tracks = selection();
        if (tracks.empty()) {
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("New list"),
                                                QStringLiteral("List name:"), QLineEdit::Normal,
                                                uniqueScratchListName(), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        QStringList uris;
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        createScratchListTab(name, uris);
    });
    submenu->addSeparator();
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        const auto target = targets[index];
        auto* tab_menu = submenu->addMenu(target.label);
        tab_menu->setObjectName(QStringLiteral("bench-send-to-tab-%1").arg(index));
        const std::array modes{
            std::pair{QStringLiteral("Add"), MpdSendMode::append},
            std::pair{QStringLiteral("Insert next"), MpdSendMode::insert_next},
            std::pair{QStringLiteral("Replace"), MpdSendMode::replace},
        };
        for (const auto& [label, mode] : modes) {
            auto* action = tab_menu->addAction(label);
            action->setEnabled(connected);
            connect(action, &QAction::triggered, this, [this, target, mode, selection] {
                auto tracks = selection();
                if (!tracks.empty()) {
                    sendTracksToMpdTab(target, std::move(tracks), mode);
                }
            });
        }
    }
}

// The tracks a drag carries, whichever server-track surface it started from:
// the library tree answers with the whole selected branch, a track table with
// its selected rows.
std::vector<mpd::Track> BenchMainWindow::mpdTracksFromSourceView(QAbstractItemView* source) const {
    if (source == nullptr || source->selectionModel() == nullptr) {
        return {};
    }
    if (source == static_cast<QAbstractItemView*>(server_library_view_)) {
        std::vector<mpd::Track> tracks;
        QSet<QString> seen;
        for (const auto& index : source->selectionModel()->selectedRows(0)) {
            for (const auto& track : server_library_model_->tracks(index)) {
                const auto uri = displayText(track.uri);
                if (!seen.contains(uri)) {
                    seen.insert(uri);
                    tracks.push_back(track);
                }
            }
        }
        return tracks;
    }
    return selectedMpdViewTracks(qobject_cast<QTableView*>(source));
}
} // namespace trackknife::bench
