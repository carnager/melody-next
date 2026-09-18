// SPDX-License-Identifier: GPL-3.0-only

// MPD stored-playlist workspace surfaces (ADR-0129): the sidebar Playlists
// list and the server-authoritative playlist tabs. Every edit is a server
// round trip; rows change only when the post-mutation re-read arrives.

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "ui/server_library_tree_model.hpp"
#include "ui/server_library_tree_view.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
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

void BenchMainWindow::buildMpdPlaylists() {
    // Full-height Playlists page behind the ADR-0130 sidebar tab bar.
    mpd_playlists_list_ = new QListWidget(mpd_source_pages_);
    mpd_playlists_list_->setObjectName(QStringLiteral("bench-mpd-playlists"));
    mpd_playlists_list_->setAccessibleName(QStringLiteral("MPD stored playlists"));
    mpd_playlists_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    mpd_playlists_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mpd_playlists_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    mpd_source_pages_->addWidget(mpd_playlists_list_);
    connect(mpd_source_tabs_, &QTabBar::currentChanged, this, [this](const int index) {
        if (mpd_source_pages_ != nullptr && index >= 0 && index < mpd_source_pages_->count()) {
            mpd_source_pages_->setCurrentIndex(index);
        }
        if (index == 1 && mpd_controller_ != nullptr && mpd_controller_->connected() &&
            mpd_controller_->supportsCommand(QStringLiteral("listplaylists"))) {
            mpd_controller_->browseStoredPlaylists();
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
        for (const auto& tab : mpd_playlist_tabs_) {
            mpd_controller_->openStoredPlaylist(tab->name);
        }
    });

    connect(mpd_playlists_list_, &QListWidget::itemActivated, this,
            [this](const QListWidgetItem* item) {
                if (item != nullptr) {
                    openMpdPlaylistTab(item->text(), true);
                }
            });
    connect(mpd_playlists_list_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showMpdPlaylistSidebarMenu);

    connect(mpd_controller_, &quick::MpdProbeController::storedPlaylistListLoaded, this,
            &BenchMainWindow::acceptMpdStoredPlaylistNames);
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

void BenchMainWindow::openMpdPlaylistTab(const QString& name, const bool select) {
    if (name.isEmpty()) {
        return;
    }
    if (auto* existing = mpdPlaylistTabNamed(name)) {
        if (select) {
            tabs_->setCurrentWidget(existing->view);
        }
        mpd_controller_->openStoredPlaylist(name);
        return;
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
    // Enter and double-click append the selected playlist rows to the live
    // queue, matching the library search contract.
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
    view->setReorderCallback([this, raw_tab](const QVariantList& rows, const int insertion_row) {
        if (rows.size() != 1) {
            statusBar()->showMessage(QStringLiteral("Stored playlists reorder one row at a time"),
                                     3'000);
            return;
        }
        const auto from = rows.front().toInt();
        if (from < 0 || insertion_row < 0) {
            return;
        }
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

    const auto index = tabs_->addTab(view, name);
    tabs_->setTabToolTip(index, QStringLiteral("Stored playlist on the connected MPD server"));
    mpd_playlist_tabs_.push_back(std::move(tab));
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    refreshTabActions();
    mpd_controller_->openStoredPlaylist(name);
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

    const auto index = tabs_->addTab(view, QStringLiteral("Search: %1").arg(query));
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
    track_context_menu_->popup(tab.view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::acceptMpdStoredPlaylistNames(const QStringList& names) {
    if (mpd_playlists_list_ == nullptr) {
        return;
    }
    const auto selected = mpd_playlists_list_->currentItem() != nullptr
                              ? mpd_playlists_list_->currentItem()->text()
                              : QString{};
    mpd_playlists_list_->clear();
    mpd_playlists_list_->addItems(names);
    if (!selected.isEmpty()) {
        const auto matches = mpd_playlists_list_->findItems(selected, Qt::MatchExactly);
        if (!matches.isEmpty()) {
            mpd_playlists_list_->setCurrentItem(matches.front());
        }
    }
}

void BenchMainWindow::acceptMpdStoredPlaylistContents(const QString& name) {
    auto* tab = mpdPlaylistTabNamed(name);
    if (tab == nullptr) {
        return;
    }
    tab->model->replaceTracks(mpd_controller_->browserPlaylistTracksSnapshot());
    refreshSelectionStatus();
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
    const auto* item = mpd_playlists_list_->itemAt(position);
    if (item != nullptr) {
        mpd_playlists_list_->setCurrentItem(const_cast<QListWidgetItem*>(item));
        addMpdPlaylistActions(mpd_playlists_menu_, item->text());
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
            [this] { mpd_controller_->browseStoredPlaylists(); });
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
