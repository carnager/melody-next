// SPDX-License-Identifier: GPL-3.0-only

// MPD stored-playlist workspace surfaces (ADR-0129): the sidebar Playlists
// list and the server-authoritative playlist tabs. Every edit is a server
// round trip; rows change only when the post-mutation re-read arrives.

#include "bench/bench_main_window.hpp"
#include "uicommon/debug_log.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTreeWidget>
#include <QSettings>
#include <QMessageBox>
#include <QSet>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

#include <tuple>
#include <array>
#include <algorithm>

namespace trackknife::bench {

namespace {
constexpr char playlist_name_property[] = "bench-mpd-playlist-name";
constexpr char search_query_property[] = "bench-mpd-search-query";
} // namespace

namespace {
// Per-item state for the playlist tree.
constexpr int playlist_loaded_role = Qt::UserRole + 101;
constexpr int playlist_track_uri_role = Qt::UserRole + 102;
} // namespace

QStringList BenchMainWindow::mpdPlaylistNames() const {
    QStringList names;
    if (mpd_playlists_list_ == nullptr) {
        return names;
    }
    names.reserve(mpd_playlists_list_->topLevelItemCount());
    for (int index = 0; index < mpd_playlists_list_->topLevelItemCount(); ++index) {
        names.push_back(mpd_playlists_list_->topLevelItem(index)->text(0));
    }
    return names;
}

void BenchMainWindow::buildMpdPlaylists() {
    // Full-height Playlists page behind the ADR-0130 sidebar tab bar.
    mpd_playlists_list_ = new QTreeWidget(mpd_source_pages_);
    mpd_playlists_list_->setObjectName(QStringLiteral("bench-mpd-playlists"));
    mpd_playlists_list_->setAccessibleName(QStringLiteral("MPD stored playlists"));
    mpd_playlists_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mpd_playlists_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mpd_playlists_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    mpd_playlists_list_->setHeaderHidden(true);
    mpd_playlists_list_->setUniformRowHeights(false);
    mpd_source_pages_->addWidget(mpd_playlists_list_);
    // Expanding a playlist fetches its tracks once; the same re-read fills
    // any open tab, so the two never disagree.
    connect(mpd_playlists_list_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* item) {
                if (item == nullptr || item->parent() != nullptr ||
                    item->data(0, playlist_loaded_role).toBool()) {
                    return;
                }
                if (mpd_controller_->connected()) {
                    mpd_controller_->openStoredPlaylist(item->text(0));
                }
            });
    connect(mpd_source_tabs_, &QTabBar::currentChanged, this, [this](const int index) {
        if (mpd_source_pages_ != nullptr && index >= 0 && index < mpd_source_pages_->count()) {
            mpd_source_pages_->setCurrentIndex(index);
        }
        if (index == 1 && mpd_controller_ != nullptr && mpd_controller_->connected() &&
            mpd_controller_->supportsCommand(QStringLiteral("listplaylists"))) {
            mpd_controller_->browseStoredPlaylists();
            mpd_controller_->browseScratchLists();
        }
    });

    mpd_playlists_menu_ = new QMenu(this);
    mpd_playlists_menu_->setObjectName(QStringLiteral("bench-mpd-playlists-menu"));

    mpd_playlists_refresh_timer_ = new QTimer(this);
    mpd_playlists_refresh_timer_->setSingleShot(true);
    mpd_playlists_refresh_timer_->setInterval(400);
    connect(mpd_playlists_refresh_timer_, &QTimer::timeout, this, [this] {
        if (mpd_controller_ == nullptr || !mpd_controller_->connected()) {
            return;
        }
        mpd_controller_->browseStoredPlaylists();
        mpd_controller_->browseScratchLists();
        for (const auto& tab : mpd_playlist_tabs_) {
            mpd_controller_->openStoredPlaylist(tab->name);
        }
    });

    connect(mpd_playlists_list_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) {
                if (item == nullptr) {
                    return;
                }
                if (item->parent() == nullptr) {
                    openMpdPlaylistTab(item->text(0), true);
                    return;
                }
                // A track row appends to the live queue, matching the
                // library tree's activation contract.
                const auto uri = item->data(0, playlist_track_uri_role).toString();
                if (!uri.isEmpty() && mpd_controller_->connected()) {
                    mpd_controller_->addUris({uri}, false);
                }
            });
    connect(mpd_playlists_list_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showMpdPlaylistSidebarMenu);

    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistListLoaded, this,
            &BenchMainWindow::acceptMpdStoredPlaylistNames);
    connect(mpd_controller_, &quick::MpdProbeController::scratchListsLoaded, this,
            &BenchMainWindow::acceptMpdScratchLists);
    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistLoaded, this,
            &BenchMainWindow::acceptMpdStoredPlaylistContents);
    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistRenamed, this,
            &BenchMainWindow::renameMpdPlaylistTab);
    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistDeleted, this,
            &BenchMainWindow::closeMpdPlaylistTab);
    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistsChanged, this,
            &BenchMainWindow::refreshMpdPlaylistsSoon);
}

BenchMainWindow::MpdPlaylistTab* BenchMainWindow::mpdPlaylistTabForWidget(QWidget* widget) const {
    if (widget == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find(mpd_playlist_tabs_, widget,
                                         [](const std::unique_ptr<MpdPlaylistTab>& tab) {
                                             return static_cast<QWidget*>(tab->view);
                                         });
    return found == mpd_playlist_tabs_.end() ? nullptr : found->get();
}

BenchMainWindow::MpdPlaylistTab* BenchMainWindow::currentMpdPlaylistTab() const {
    return tabs_ == nullptr ? nullptr : mpdPlaylistTabForWidget(tabs_->currentWidget());
}

BenchMainWindow::MpdPlaylistTab* BenchMainWindow::mpdPlaylistTabNamed(const QString& name) const {
    const auto found = std::ranges::find(mpd_playlist_tabs_, name, &MpdPlaylistTab::name);
    return found == mpd_playlist_tabs_.end() ? nullptr : found->get();
}

BenchMainWindow::MpdPlaylistTab* BenchMainWindow::openMpdPlaylistTab(const QString& name,
                                                                    const bool select) {
    if (name.isEmpty()) {
        return nullptr;
    }
    if (auto* existing = mpdPlaylistTabNamed(name)) {
        if (select) {
            tabs_->setCurrentWidget(existing->view);
        }
        mpd_controller_->openStoredPlaylist(name);
        return existing;
    }

    auto tab = std::make_unique<MpdPlaylistTab>();
    tab->name = name;
    tab->model = new quick::MpdQueueModel(this);
    tab->model->setArtworkEnabled(true);
    connect(tab->model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);

    auto* view = new ui::QueueTableView(tabs_);
    tab->view = view;
    view->setObjectName(QStringLiteral("bench-mpd-playlist-view"));
    view->setProperty(playlist_name_property, name);
    view->setAccessibleName(QStringLiteral("MPD stored playlist %1").arg(name));
    view->setModel(tab->model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* raw_tab = tab.get();
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    // ADR-0187: Enter and double-click play the list from that row, the way
    // a local list plays. Against a server without playback contexts the
    // list replaces the queue and starts there instead, so the gesture
    // means the same thing everywhere.
    const auto play_from_row = [this, raw_tab](const QModelIndex& index) {
        if (!index.isValid()) {
            return;
        }
        if (mpd_controller_->supportsPlaybackContexts()) {
            mpd_controller_->playStoredPlaylistContext(raw_tab->name, index.row());
            return;
        }
        QStringList uris;
        const auto tracks = raw_tab->model->tracksSnapshot();
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        if (!uris.isEmpty()) {
            mpd_controller_->replaceQueueWithUrisAndPlayAt(uris, index.row());
        }
    };
    view->setActivateCallback(play_from_row);
    connect(view, &QTableView::doubleClicked, this, play_from_row);
    view->setReorderCallback([this, raw_tab](const QVariantList& rows, const int insertion_row) {
        if (rows.isEmpty() || insertion_row < 0) {
            return;
        }
        // ADR-0187: a multi-row drag is a sequence of single moves; each one
        // shifts the indices the next one addresses, so the block is walked
        // in order with the running offset applied.
        std::vector<int> sources;
        sources.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& value : rows) {
            if (const auto row = value.toInt(); row >= 0) {
                sources.push_back(row);
            }
        }
        std::ranges::sort(sources);
        if (sources.size() > 1U) {
            auto target = insertion_row;
            for (const auto source : sources) {
                if (source < target) {
                    --target;
                }
            }
            for (std::size_t index = 0U; index < sources.size(); ++index) {
                auto from = sources[index];
                for (std::size_t seen = 0U; seen < index; ++seen) {
                    if (sources[seen] < sources[index]) {
                        --from;
                    }
                }
                const auto to = target + static_cast<int>(index);
                if (from != to) {
                    mpd_controller_->moveStoredPlaylistItem(raw_tab->name, from, to);
                }
            }
            return;
        }
        const auto from = sources.front();
        const auto target = insertion_row > from ? insertion_row - 1 : insertion_row;
        if (target != from) {
            mpd_controller_->moveStoredPlaylistItem(raw_tab->name, from, target);
        }
    });
    view->setExternalDropCallback([this, raw_tab](QAbstractItemView* source, const QVariantList&,
                                                  const int insertion_row, const Qt::DropAction) {
        QStringList uris;
        if (source == server_library_view_ && source->selectionModel() != nullptr) {
            QSet<QString> seen;
            for (const auto& index : source->selectionModel()->selectedRows(0)) {
                for (const auto& track : server_library_model_->tracks(index)) {
                    const auto uri = displayText(track.uri);
                    if (!seen.contains(uri)) {
                        seen.insert(uri);
                        uris.push_back(uri);
                    }
                }
            }
        } else if (auto* table = qobject_cast<QTableView*>(source)) {
            uris = selectedMpdViewUris(table);
        }
        if (uris.isEmpty()) {
            return false;
        }
        mpd_controller_->addToStoredPlaylist(raw_tab->name, uris, insertion_row);
        return true;
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(tab->model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });

    tab->view_layout = mpd_view_layout_;
    applyTrackViewLayout(view, tab->view_layout, mpd_view_layout_);

    const auto index = tabs_->insertTab(
        mpdTabInsertionIndex(), view, QIcon::fromTheme(QStringLiteral("network-server")), name);
    tabs_->setTabToolTip(index, QStringLiteral("Stored playlist on the connected MPD server"));
    tab->scratch = mpd_scratch_lists_.contains(name);
    auto* opened = tab.get();
    mpd_playlist_tabs_.push_back(std::move(tab));
    refreshMpdPlaylistTabChrome(*opened);
    persistOpenPlaylistTabs();
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    refreshTabActions();
    mpd_controller_->openStoredPlaylist(name);
    return opened;
}

// A scratch tab reads as a working list, a playlist tab as the stored
// playlist it is — same object either way, different presentation.
void BenchMainWindow::refreshMpdPlaylistTabChrome(MpdPlaylistTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    tabs_->setTabIcon(index, QIcon::fromTheme(tab.scratch ? QStringLiteral("view-list-text")
                                                          : QStringLiteral("network-server")));
    tabs_->setTabToolTip(index, tab.scratch
                                    ? QStringLiteral("Working list on the connected MPD server")
                                    : QStringLiteral("Stored playlist on the connected MPD server"));
}

// ADR-0140: committed search-result tabs. Query-keyed snapshots of a
// finished library search, session-only like stored-playlist tabs, but
// read-only: rows only feed the live queue.
BenchMainWindow::MpdSearchTab* BenchMainWindow::mpdSearchTabForWidget(QWidget* widget) const {
    if (widget == nullptr) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(mpd_search_tabs_, widget, [](const std::unique_ptr<MpdSearchTab>& tab) {
            return static_cast<QWidget*>(tab->view);
        });
    return found == mpd_search_tabs_.end() ? nullptr : found->get();
}

BenchMainWindow::MpdSearchTab* BenchMainWindow::currentMpdSearchTab() const {
    return tabs_ == nullptr ? nullptr : mpdSearchTabForWidget(tabs_->currentWidget());
}

BenchMainWindow::MpdSearchTab* BenchMainWindow::mpdSearchTabForQuery(const QString& query) const {
    const auto found = std::ranges::find(mpd_search_tabs_, query, &MpdSearchTab::query);
    return found == mpd_search_tabs_.end() ? nullptr : found->get();
}

void BenchMainWindow::commitMpdSearchTab() {
    if (mpd_controller_ == nullptr || mpd_search_field_ == nullptr) {
        return;
    }
    const auto query = mpd_search_field_->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    // Committing snapshots the displayed hits; it needs finished results,
    // not a live connection. Enter that outruns the debounce or the
    // server reply parks the commit until finishMpdSearch delivers.
    if (mpd_search_timer_->isActive() || mpd_controller_->libraryBusy() ||
        (mpd_controller_->connected() && query != mpd_controller_->lastSearchQuery())) {
        mpd_search_timer_->stop();
        pending_mpd_search_commit_ = query;
        previewMpdSearch();
        return;
    }
    auto tracks = mpd_controller_->libraryTracksSnapshot();
    if (tracks.empty()) {
        statusBar()->showMessage(QStringLiteral("No search results to keep"), 3'000);
        return;
    }
    openMpdSearchTab(query, std::move(tracks), true);
}

void BenchMainWindow::openMpdSearchTab(const QString& query, std::vector<mpd::Track> tracks,
                                       const bool select) {
    if (auto* existing = mpdSearchTabForQuery(query)) {
        existing->model->replaceTracks(std::move(tracks));
        if (select) {
            tabs_->setCurrentWidget(existing->view);
        }
        refreshSelectionStatus();
        return;
    }

    auto tab = std::make_unique<MpdSearchTab>();
    tab->query = query;
    tab->model = new quick::MpdQueueModel(this);
    tab->model->setArtworkEnabled(true);
    connect(tab->model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);

    auto* view = new ui::QueueTableView(tabs_);
    tab->view = view;
    view->setObjectName(QStringLiteral("bench-mpd-search-tab-view"));
    view->setProperty(search_query_property, query);
    view->setAccessibleName(QStringLiteral("MPD search results %1").arg(query));
    view->setModel(tab->model);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->setDragEnabled(true);
    view->setDragDropMode(QAbstractItemView::DragOnly);
    view->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* raw_tab = tab.get();
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    // Enter and double-click append the selection to the live queue — the
    // same contract as stored-playlist tabs and the live search surface.
    view->setActivateCallback([this, raw_tab](const QModelIndex& index) {
        if (index.isValid()) {
            const auto uris = selectedMpdViewUris(raw_tab->view);
            if (!uris.isEmpty()) {
                mpd_controller_->addUris(uris, false);
            }
        }
    });
    connect(view, &QTableView::doubleClicked, this, [this, raw_tab](const QModelIndex& index) {
        if (index.isValid()) {
            const auto uris = selectedMpdViewUris(raw_tab->view);
            if (!uris.isEmpty()) {
                mpd_controller_->addUris(uris, false);
            }
        }
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(tab->model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });

    tab->view_layout = mpd_view_layout_;
    applyTrackViewLayout(view, tab->view_layout, mpd_view_layout_);
    tab->model->replaceTracks(std::move(tracks));

    const auto index =
        tabs_->insertTab(mpdTabInsertionIndex(), view,
                         QIcon::fromTheme(QStringLiteral("network-server")),
                         QStringLiteral("Search: %1").arg(query));
    tabs_->setTabToolTip(index,
                         QStringLiteral("Committed MPD search results (snapshot of the query)"));
    mpd_search_tabs_.push_back(std::move(tab));
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    refreshTabActions();
}

void BenchMainWindow::closeMpdSearchTab(MpdSearchTab* tab) {
    if (tab == nullptr) {
        return;
    }
    const auto index = tabs_->indexOf(tab->view);
    if (index >= 0) {
        tabs_->removeTab(index);
    }
    tab->view->deleteLater();
    tab->model->deleteLater();
    std::erase_if(mpd_search_tabs_,
                  [tab](const std::unique_ptr<MpdSearchTab>& owned) { return owned.get() == tab; });
    refreshTabActions();
}


// ADR-0180: shared "Load as local files" + file-operation sugar entries for
// the MPD playlist and committed-search track menus. These act on mapped
// local files only and never talk to MPD, so they ignore command readiness.
void BenchMainWindow::addMappedLocalTrackActions(QMenu* menu, const QStringList& uris,
                                                 const QString& object_prefix) {
    auto* load_local = menu->addAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                                       QStringLiteral("Load as local files"));
    load_local->setObjectName(object_prefix + QStringLiteral("load-local"));
    load_local->setEnabled(!uris.isEmpty());
    connect(load_local, &QAction::triggered, this,
            [this, uris] { loadMpdUrisAsLocalFiles(uris); });
    const auto mapped_ready = !uris.isEmpty() && !effectiveMpdMusicRoot().isEmpty();
    const std::array sugar{
        std::tuple{QStringLiteral("Edit tags…"), QStringLiteral("edit-tags"),
                   MaterializedDialog::edit_tags},
        std::tuple{QStringLiteral("ReplayGain…"), QStringLiteral("replaygain"),
                   MaterializedDialog::replay_gain},
        std::tuple{QStringLiteral("Convert files…"), QStringLiteral("convert"),
                   MaterializedDialog::convert},
    };
    for (const auto& [label, slug, dialog] : sugar) {
        auto* command = menu->addAction(label);
        command->setObjectName(object_prefix + slug);
        command->setEnabled(mapped_ready);
        connect(command, &QAction::triggered, this,
                [this, uris, dialog] { materializeMpdSelectionForDialog(uris, dialog); });
    }
}

void BenchMainWindow::showMpdSearchTrackMenu(MpdSearchTab& tab, const QPoint& position) {
    if (track_context_menu_ == nullptr) {
        return;
    }
    const auto target = tab.view->indexAt(position);
    if (target.isValid() && tab.view->selectionModel() != nullptr &&
        !tab.view->selectionModel()->isRowSelected(target.row(), target.parent())) {
        tab.view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        tab.view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();
    const auto command_ready = mpd_controller_->connected() && !mpd_controller_->commandBusy();
    const auto uris = selectedMpdViewUris(tab.view);

    track_context_menu_->clear();
    auto* append = track_context_menu_->addAction(QStringLiteral("Append to live queue"));
    append->setObjectName(QStringLiteral("action-mpd-search-append-selection"));
    append->setEnabled(command_ready && !uris.isEmpty());
    connect(append, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, false); });
    auto* next = track_context_menu_->addAction(QStringLiteral("Insert next in live queue"));
    next->setObjectName(QStringLiteral("action-mpd-search-next-selection"));
    next->setEnabled(command_ready && !uris.isEmpty());
    connect(next, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, true); });
    auto* replace = track_context_menu_->addAction(QStringLiteral("Replace queue and play"));
    replace->setObjectName(QStringLiteral("action-mpd-search-replace-selection"));
    replace->setEnabled(command_ready && !uris.isEmpty());
    connect(replace, &QAction::triggered, this,
            [this, uris] { mpd_controller_->replaceQueueWithUris(uris); });
    track_context_menu_->addSeparator();
    addMappedLocalTrackActions(track_context_menu_, uris, QStringLiteral("action-mpd-search-"));
    addSendToTabMenu(track_context_menu_, [this, view = tab.view] {
        return selectedMpdViewTracks(view);
    });
    addCopyToServerListMenu(track_context_menu_, tab.view);
    track_context_menu_->popup(tab.view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::acceptMpdStoredPlaylistNames(const QStringList& names) {
    mpd_playlist_names_ = names;
    if (mpd_playlists_list_ == nullptr) {
        return;
    }
    const auto* current = mpd_playlists_list_->currentItem();
    const auto selected = current != nullptr && current->parent() == nullptr
                              ? current->text(0)
                              : QString{};
    QStringList expanded;
    for (int index = 0; index < mpd_playlists_list_->topLevelItemCount(); ++index) {
        auto* item = mpd_playlists_list_->topLevelItem(index);
        if (item->isExpanded()) {
            expanded.push_back(item->text(0));
        }
    }
    mpd_playlists_list_->clear();
    // ADR-0191: working lists are stored playlists too, but they belong in
    // the tab strip; the sidebar stays the curated-playlist view.
    for (const auto& name : names) {
        if (mpd_scratch_lists_.contains(name)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(mpd_playlists_list_, {name});
        item->setIcon(0, QIcon::fromTheme(QStringLiteral("view-media-playlist")));
        // A child placeholder makes the expander appear before the tracks
        // have been fetched.
        item->addChild(new QTreeWidgetItem({QStringLiteral("Loading…")}));
        if (expanded.contains(name)) {
            mpd_playlists_list_->expandItem(item);
        }
        if (name == selected) {
            mpd_playlists_list_->setCurrentItem(item);
        }
    }
    restoreOpenPlaylistTabs(names);
}

// The server's scratch flags arrived: tabs and sidebar both follow them.
void BenchMainWindow::acceptMpdScratchLists(const QStringList& names) {
    const auto changed = mpd_scratch_lists_ != QSet<QString>{names.begin(), names.end()};
    mpd_scratch_lists_ = QSet<QString>{names.begin(), names.end()};
    if (!changed) {
        return;
    }
    for (const auto& tab : mpd_playlist_tabs_) {
        tab->scratch = mpd_scratch_lists_.contains(tab->name);
        refreshMpdPlaylistTabChrome(*tab);
    }
    acceptMpdStoredPlaylistNames(mpd_playlist_names_);
}

// ADR-0187: the playlist tab whose list is the active context marks the
// playing row; every other tab clears its marker.
void BenchMainWindow::refreshMpdPlaylistContextMarkers() {
    const auto active = mpd_controller_->activeContextName();
    const auto position = mpd_controller_->songPosition();
    for (const auto& tab : mpd_playlist_tabs_) {
        const auto playing = !active.isEmpty() && tab->name == active && position >= 0;
        tab->model->setCurrentRow(playing ? std::optional{position} : std::nullopt);
    }
}

void BenchMainWindow::acceptMpdStoredPlaylistContents(const QString& name) {
    // A re-read serves both surfaces: an open tab, an expanded sidebar row,
    // or both. Either may be absent, so neither gates the other.
    if (auto* tab = mpdPlaylistTabNamed(name); tab != nullptr) {
        tab->model->replaceTracks(mpd_controller_->browserPlaylistTracksSnapshot());
        refreshSelectionStatus();
        refreshMpdPlaylistContextMarkers();
    }
    // ADR-0189: fill the sidebar's expanded playlist with its tracks.
    if (mpd_playlists_list_ != nullptr) {
        for (int index = 0; index < mpd_playlists_list_->topLevelItemCount(); ++index) {
            auto* item = mpd_playlists_list_->topLevelItem(index);
            if (item->text(0) != name) {
                continue;
            }
            const auto tracks = mpd_controller_->browserPlaylistTracksSnapshot();
            item->takeChildren();
            for (const auto& track : tracks) {
                const auto title = track.metadata.first("Title");
                const auto artist = track.metadata.first("Artist");
                auto label = title ? displayText(std::string{*title}) : displayText(track.uri);
                if (artist) {
                    label = QStringLiteral("%1 — %2").arg(label,
                                                          displayText(std::string{*artist}));
                }
                auto* row = new QTreeWidgetItem(item, {label});
                row->setData(0, playlist_track_uri_role, displayText(track.uri));
                row->setToolTip(0, displayText(track.uri));
            }
            item->setData(0, playlist_loaded_role, true);
            item->setToolTip(0, tracks.size() == 1U
                                    ? QStringLiteral("1 track")
                                    : QStringLiteral("%1 tracks").arg(tracks.size()));
            break;
        }
    }
}

void BenchMainWindow::renameMpdPlaylistTab(const QString& from, const QString& to) {
    auto* tab = mpdPlaylistTabNamed(from);
    if (tab == nullptr) {
        return;
    }
    tab->name = to;
    tab->view->setProperty(playlist_name_property, to);
    tab->view->setAccessibleName(QStringLiteral("MPD stored playlist %1").arg(to));
    const auto index = tabs_->indexOf(tab->view);
    if (index >= 0) {
        tabs_->setTabText(index, to);
    }
    persistOpenPlaylistTabs();
}

// ADR-0187: server list tabs survive restarts like local list tabs. Only
// the names persist; contents always come from the authoritative re-read.
void BenchMainWindow::persistOpenPlaylistTabs() {
    QStringList names;
    names.reserve(static_cast<qsizetype>(mpd_playlist_tabs_.size()));
    for (const auto& tab : mpd_playlist_tabs_) {
        names.push_back(tab->name);
    }
    QSettings{}.setValue(QStringLiteral("mpd/open-playlist-tabs"), names);
}

void BenchMainWindow::restoreOpenPlaylistTabs(const QStringList& available) {
    const auto names = QSettings{}.value(QStringLiteral("mpd/open-playlist-tabs")).toStringList();
    qCDebug(tkDebug) << "restoring playlist tabs" << names << "available on the server" << available;
    for (const auto& name : names) {
        if (available.contains(name) && mpdPlaylistTabNamed(name) == nullptr) {
            openMpdPlaylistTab(name, false);
        }
    }
}

void BenchMainWindow::closeMpdPlaylistTab(const QString& name) {
    auto* tab = mpdPlaylistTabNamed(name);
    if (tab == nullptr) {
        return;
    }
    const auto index = tabs_->indexOf(tab->view);
    if (index >= 0) {
        tabs_->removeTab(index);
    }
    tab->view->deleteLater();
    tab->model->deleteLater();
    std::erase_if(mpd_playlist_tabs_, [tab](const std::unique_ptr<MpdPlaylistTab>& owned) {
        return owned.get() == tab;
    });
    refreshTabActions();
    persistOpenPlaylistTabs();
}

void BenchMainWindow::refreshMpdPlaylistsSoon() {
    if (mpd_playlists_refresh_timer_ != nullptr) {
        mpd_playlists_refresh_timer_->start();
    }
}

void BenchMainWindow::addMpdPlaylistActions(QMenu* menu, const QString& name) {
    auto* open = menu->addAction(QStringLiteral("Open"));
    open->setObjectName(QStringLiteral("action-mpd-playlist-open"));
    open->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("listplaylistinfo")));
    connect(open, &QAction::triggered, this, [this, name] { openMpdPlaylistTab(name, true); });

    auto* load = menu->addAction(QStringLiteral("Load into queue"));
    load->setObjectName(QStringLiteral("action-mpd-playlist-load"));
    load->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("load")));
    connect(load, &QAction::triggered, this,
            [this, name] { mpd_controller_->loadStoredPlaylistIntoQueue(name); });

    menu->addSeparator();
    auto* rename = menu->addAction(QStringLiteral("Rename…"));
    rename->setObjectName(QStringLiteral("action-mpd-playlist-rename"));
    rename->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("rename")));
    connect(rename, &QAction::triggered, this, [this, name] { promptRenameMpdPlaylist(name); });

    auto* clear = menu->addAction(QStringLiteral("Clear…"));
    clear->setObjectName(QStringLiteral("action-mpd-playlist-clear"));
    clear->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("playlistclear")));
    connect(clear, &QAction::triggered, this, [this, name] { confirmClearMpdPlaylist(name); });

    auto* remove = menu->addAction(QStringLiteral("Delete…"));
    remove->setObjectName(QStringLiteral("action-mpd-playlist-delete"));
    remove->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("rm")));
    connect(remove, &QAction::triggered, this, [this, name] { confirmDeleteMpdPlaylist(name); });
}

void BenchMainWindow::showMpdPlaylistSidebarMenu(const QPoint& position) {
    if (mpd_playlists_menu_ == nullptr || mpd_playlists_list_ == nullptr) {
        return;
    }
    mpd_playlists_menu_->clear();
    auto* item = mpd_playlists_list_->itemAt(position);
    if (item != nullptr) {
        mpd_playlists_list_->setCurrentItem(item);
        if (item->parent() == nullptr) {
            addMpdPlaylistActions(mpd_playlists_menu_, item->text(0));
        } else {
            // A track row inside an expanded playlist: the usual queue
            // gestures, addressed by URI.
            QStringList uris;
            for (auto* selected : mpd_playlists_list_->selectedItems()) {
                const auto uri = selected->data(0, playlist_track_uri_role).toString();
                if (!uri.isEmpty()) {
                    uris.push_back(uri);
                }
            }
            if (uris.isEmpty()) {
                uris.push_back(item->data(0, playlist_track_uri_role).toString());
            }
            const auto ready = mpd_controller_->connected() && !uris.isEmpty();
            const auto add_action = [this, &uris, ready](const QString& label,
                                                         const QString& object_name,
                                                         const bool next) {
                auto* action = mpd_playlists_menu_->addAction(label);
                action->setObjectName(object_name);
                action->setEnabled(ready);
                connect(action, &QAction::triggered, this,
                        [this, uris, next] { mpd_controller_->addUris(uris, next); });
            };
            add_action(QStringLiteral("Append to live queue"),
                       QStringLiteral("action-mpd-playlist-track-append"), false);
            add_action(QStringLiteral("Insert next in live queue"),
                       QStringLiteral("action-mpd-playlist-track-next"), true);
            auto* replace = mpd_playlists_menu_->addAction(
                QStringLiteral("Replace queue and play"));
            replace->setObjectName(QStringLiteral("action-mpd-playlist-track-replace"));
            replace->setEnabled(ready);
            connect(replace, &QAction::triggered, this,
                    [this, uris] { mpd_controller_->replaceQueueWithUris(uris); });
        }
        mpd_playlists_menu_->addSeparator();
    }
    auto* save = mpd_playlists_menu_->addAction(QStringLiteral("Save queue as playlist…"));
    save->setObjectName(QStringLiteral("action-mpd-playlist-save-queue"));
    save->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("save")));
    connect(save, &QAction::triggered, this, &BenchMainWindow::promptSaveQueueAsPlaylist);
    auto* refresh = mpd_playlists_menu_->addAction(QStringLiteral("Refresh"));
    refresh->setObjectName(QStringLiteral("action-mpd-playlist-refresh"));
    refresh->setEnabled(mpd_controller_->connected());
    connect(refresh, &QAction::triggered, this,
            [this] {
                mpd_controller_->browseStoredPlaylists();
                mpd_controller_->browseScratchLists();
            });
    mpd_playlists_menu_->popup(mpd_playlists_list_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::showMpdPlaylistTrackMenu(MpdPlaylistTab& tab, const QPoint& position) {
    if (track_context_menu_ == nullptr) {
        return;
    }
    const auto target = tab.view->indexAt(position);
    if (target.isValid() && tab.view->selectionModel() != nullptr &&
        !tab.view->selectionModel()->isRowSelected(target.row(), target.parent())) {
        tab.view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        tab.view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();
    const auto command_ready = mpd_controller_->connected() && !mpd_controller_->commandBusy();
    const auto uris = selectedMpdViewUris(tab.view);
    const auto name = tab.name;

    track_context_menu_->clear();
    auto* append = track_context_menu_->addAction(QStringLiteral("Append to live queue"));
    append->setObjectName(QStringLiteral("action-mpd-playlist-append-selection"));
    append->setEnabled(command_ready && !uris.isEmpty());
    connect(append, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, false); });
    auto* next = track_context_menu_->addAction(QStringLiteral("Insert next in live queue"));
    next->setObjectName(QStringLiteral("action-mpd-playlist-next-selection"));
    next->setEnabled(command_ready && !uris.isEmpty());
    connect(next, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, true); });
    track_context_menu_->addSeparator();
    addMappedLocalTrackActions(track_context_menu_, uris, QStringLiteral("action-mpd-playlist-"));
    addSendToTabMenu(track_context_menu_, [this, view = tab.view] {
        return selectedMpdViewTracks(view);
    });
    addCopyToServerListMenu(track_context_menu_, tab.view);
    track_context_menu_->addSeparator();
    auto* remove = track_context_menu_->addAction(QStringLiteral("Remove from playlist"));
    remove->setObjectName(QStringLiteral("action-mpd-playlist-remove-selection"));
    remove->setEnabled(command_ready && tab.view->selectionModel() != nullptr &&
                       !tab.view->selectionModel()->selectedRows().isEmpty() &&
                       mpd_controller_->supportsCommand(QStringLiteral("playlistdelete")));
    connect(remove, &QAction::triggered, this, [this, name] {
        auto* current = mpdPlaylistTabNamed(name);
        if (current == nullptr || current->view->selectionModel() == nullptr) {
            return;
        }
        QVariantList rows;
        for (const auto& index : current->view->selectionModel()->selectedRows()) {
            rows.push_back(index.row());
        }
        if (!rows.isEmpty()) {
            mpd_controller_->removeStoredPlaylistItems(name, rows);
        }
    });
    track_context_menu_->addSeparator();
    addMpdPlaylistActions(track_context_menu_, name);
    track_context_menu_->popup(tab.view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::promptSaveQueueAsPlaylist() {
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Save queue as playlist"),
                                            QStringLiteral("Playlist name:"), QLineEdit::Normal,
                                            QString{}, &accepted)
                          .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    mpd_controller_->saveQueueAsPlaylist(name);
}

void BenchMainWindow::promptRenameMpdPlaylist(const QString& name) {
    bool accepted = false;
    const auto renamed =
        QInputDialog::getText(this, QStringLiteral("Rename playlist"), QStringLiteral("Name:"),
                              QLineEdit::Normal, name, &accepted)
            .trimmed();
    if (!accepted || renamed.isEmpty() || renamed == name) {
        return;
    }
    mpd_controller_->renameStoredPlaylist(name, renamed);
}

void BenchMainWindow::confirmClearMpdPlaylist(const QString& name) {
    QMessageBox confirmation{
        QMessageBox::Question,
        QStringLiteral("Clear stored playlist"),
        QStringLiteral("Remove every entry of “%1” on the server?").arg(name),
        QMessageBox::Yes | QMessageBox::No,
        this,
    };
    confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
    confirmation.setDefaultButton(QMessageBox::No);
    if (confirmation.exec() == QMessageBox::Yes) {
        mpd_controller_->clearStoredPlaylist(name);
    }
}

void BenchMainWindow::confirmDeleteMpdPlaylist(const QString& name) {
    QMessageBox confirmation{
        QMessageBox::Question,
        QStringLiteral("Delete stored playlist"),
        QStringLiteral("Delete “%1” from the server?").arg(name),
        QMessageBox::Yes | QMessageBox::No,
        this,
    };
    confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
    confirmation.setDefaultButton(QMessageBox::No);
    if (confirmation.exec() == QMessageBox::Yes) {
        mpd_controller_->deleteStoredPlaylist(name);
    }
}

QStringList BenchMainWindow::selectedMpdViewUris(QTableView* view) const {
    QStringList uris;
    if (view == nullptr || view->selectionModel() == nullptr) {
        return uris;
    }
    const auto* model = qobject_cast<const quick::MpdQueueModel*>(view->model());
    if (model == nullptr) {
        return uris;
    }
    auto selected = view->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    for (const auto& index : selected) {
        if (const auto uri = model->uriAt(index.row())) {
            uris.push_back(displayText(*uri));
        }
    }
    return uris;
}

} // namespace trackknife::bench
