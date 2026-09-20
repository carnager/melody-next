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
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <tuple>

namespace trackknife::bench {

namespace {
constexpr char playlist_name_property[] = "bench-mpd-playlist-name";
} // namespace

namespace {
// Per-item state for the playlist tree.
constexpr int playlist_loaded_role = Qt::UserRole + 101;
constexpr int playlist_track_uri_role = Qt::UserRole + 102;
} // namespace

// Every stored playlist the server has, working lists included. Name
// collisions are decided here: the sidebar hides scratch lists, so reading
// names off it would let a "new" list reuse an existing one's name and
// quietly append to it.
QStringList BenchMainWindow::mpdPlaylistNames() const { return mpd_playlist_names_; }

// The curated ones only — what "Add to playlist" offers.
QStringList BenchMainWindow::curatedPlaylistNames() const {
    QStringList names;
    names.reserve(mpd_playlist_names_.size());
    for (const auto& name : mpd_playlist_names_) {
        if (!mpd_scratch_lists_.contains(name)) {
            names.push_back(name);
        }
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
    connect(mpd_playlists_list_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
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
                // A sidebar track belongs to its server list, just like its tab.
                if (mpd_controller_->connected())
                    mpd_controller_->playListContext(item->parent()->text(0),
                                                     item->parent()->indexOfChild(item));
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
            [this](const QString& name) {
                setMpdDynamicSnapshot(name, false);
                closeMpdPlaylistTab(name);
            });
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
        mpd_controller_->playListContext(raw_tab->name, index.row());
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
        if (source ==
                static_cast<QAbstractItemView*>(static_cast<QTreeView*>(server_library_view_)) &&
            source->selectionModel() != nullptr) {
            // The branch may not be fetched yet — an unexpanded artist is
            // the ordinary case — so the drop finishes when its rows land.
            const auto target_name = raw_tab->name;
            return resolveLibraryTracks(
                source->selectionModel()->selectedRows(0),
                [this, target_name, insertion_row](std::vector<mpd::Track> tracks) {
                    QStringList resolved;
                    for (const auto& track : tracks) {
                        resolved.push_back(displayText(track.uri));
                    }
                    if (!resolved.isEmpty()) {
                        mpd_controller_->addToStoredPlaylist(target_name, resolved, insertion_row);
                    }
                });
        }
        if (auto* table = qobject_cast<QTableView*>(source)) {
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

    const auto layout = mpdDynamicSnapshots().contains(name)
                            ? defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns)
                            : mpd_view_layout_;
    applyTrackViewLayout(view, tab->view_layout, layout);

    const auto index = tabs_->insertTab(mpdTabInsertionIndex(), view,
                                        QIcon::fromTheme(QStringLiteral("network-server")), name);
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
    const auto active =
        mpd_controller_->connected() && mpd_controller_->activeContextName() == tab.name;
    tabs_->setTabText(index, tab.name + (active ? tr(" · Active") : QString{}));
    tabs_->tabBar()->setTabTextColor(index, active ? tabs_->palette().color(QPalette::Highlight)
                                                   : QColor{});
    tabs_->tabBar()->setTabData(index, active);
    tabs_->setTabIcon(index, QIcon::fromTheme(active        ? QStringLiteral("media-playback-start")
                                              : tab.scratch ? QStringLiteral("view-list-text")
                                                            : QStringLiteral("network-server")));
    tabs_->setTabToolTip(
        index, tab.scratch ? QStringLiteral("Working list on the connected MPD server")
                           : QStringLiteral("Stored playlist on the connected MPD server"));
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

// ADR-0192: a committed search is a list like any other — it is written to
// the server as a working list, so it can be edited, reordered, played and
// added to exactly like the tabs beside it. Searching the same thing again
// refreshes that list in place.
void BenchMainWindow::openMpdSearchTab(const QString& query, std::vector<mpd::Track> tracks,
                                       const bool select) {
    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));
    for (const auto& track : tracks) {
        uris.push_back(displayText(track.uri));
    }
    createScratchListTab(query, uris, select);
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
    connect(load_local, &QAction::triggered, this, [this, uris] { loadMpdUrisAsLocalFiles(uris); });
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

void BenchMainWindow::acceptMpdStoredPlaylistNames(const QStringList& names) {
    mpd_playlist_names_ = names;
    refreshMpdPlaylistSidebar();
    restoreOpenPlaylistTabs(names);
}

void BenchMainWindow::refreshMpdPlaylistSidebar() {
    if (mpd_playlists_list_ == nullptr) {
        return;
    }
    const auto* current = mpd_playlists_list_->currentItem();
    const auto selected =
        current != nullptr && current->parent() == nullptr ? current->text(0) : QString{};
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
    for (const auto& name : mpd_playlist_names_) {
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
    refreshMpdPlaylistSidebar();
}

// ADR-0187: the playlist tab whose list is the active context marks the
// playing row; every other tab clears its marker.
void BenchMainWindow::refreshMpdPlaylistContextMarkers() {
    const auto active = mpd_controller_->activeContextName();
    const auto position = mpd_controller_->songPosition();
    const auto queue_index = tabs_->indexOf(mpd_queue_view_);
    if (queue_index >= 0) {
        const auto queue_active =
            mpd_controller_->connected() && active.isEmpty() && !mpd_controller_->queueStashed();
        tabs_->setTabText(queue_index,
                          tr("MPD Queue") + (queue_active ? tr(" · Active") : QString{}));
        tabs_->tabBar()->setTabTextColor(
            queue_index, queue_active ? tabs_->palette().color(QPalette::Highlight) : QColor{});
        tabs_->tabBar()->setTabData(queue_index, queue_active);
        tabs_->setTabToolTip(
            queue_index,
            queue_active
                ? tr("Active server playback list; selected tabs only change what you browse")
                : tr("Server queue"));
    }
    for (const auto& tab : mpd_playlist_tabs_) {
        refreshMpdPlaylistTabChrome(*tab);
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
        if (pending_playing_list_ == name) {
            pending_playing_list_.clear();
            if (tabs_->currentWidget() == tab->view)
                refreshPlaybackCursor(true);
        }
    }
    // ADR-0188: fill the sidebar's expanded playlist with its tracks.
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
                    label = QStringLiteral("%1 — %2").arg(label, displayText(std::string{*artist}));
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
    if (mpdDynamicSnapshots().contains(from)) {
        setMpdDynamicSnapshot(from, false);
        setMpdDynamicSnapshot(to, true);
    }
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
    qCDebug(tkDebug) << "restoring playlist tabs" << names << "available on the server"
                     << available;
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

    auto* play = menu->addAction(QStringLiteral("Play list"));
    play->setObjectName(QStringLiteral("action-mpd-playlist-play"));
    play->setEnabled(mpd_controller_->connected());
    connect(play, &QAction::triggered, this,
            [this, name] { mpd_controller_->playListContext(name, 0); });

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
            for (const bool next : {true, false}) {
                auto* action = mpd_playlists_menu_->addAction(
                    next ? QStringLiteral("Queue next") : QStringLiteral("Queue at end"));
                action->setEnabled(
                    ready && mpd_controller_->supportsCommand(QStringLiteral("melody_upnext")));
                connect(action, &QAction::triggered, this, [this, uris, next] {
                    mpd::RequestQueueCommand request;
                    request.operation = next ? mpd::RequestQueueOperation::prepend
                                             : mpd::RequestQueueOperation::append;
                    for (const auto& uri : uris)
                        request.uris.push_back(uri.toStdString());
                    mpd_controller_->editRequestQueue(std::move(request));
                });
            }
            addSendToTabMenu(mpd_playlists_menu_, [uris] {
                std::vector<mpd::Track> tracks;
                for (const auto& uri : uris) {
                    mpd::Track track;
                    track.uri = uri.toStdString();
                    tracks.push_back(std::move(track));
                }
                return tracks;
            });
        }
        mpd_playlists_menu_->addSeparator();
    }
    auto* save =
        mpd_playlists_menu_->addAction(QStringLiteral("Save active playback as playlist…"));
    save->setObjectName(QStringLiteral("action-mpd-playlist-save-queue"));
    save->setEnabled(mpd_controller_->supportsCommand(QStringLiteral("save")));
    connect(save, &QAction::triggered, this, &BenchMainWindow::promptSaveQueueAsPlaylist);
    auto* refresh = mpd_playlists_menu_->addAction(QStringLiteral("Refresh"));
    refresh->setObjectName(QStringLiteral("action-mpd-playlist-refresh"));
    refresh->setEnabled(mpd_controller_->connected());
    connect(refresh, &QAction::triggered, this, [this] {
        mpd_controller_->browseStoredPlaylists();
        mpd_controller_->browseScratchLists();
    });
    mpd_playlists_menu_->popup(mpd_playlists_list_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::promptSaveQueueAsPlaylist() {
    bool accepted = false;
    const auto name = QInputDialog::getText(
                          this, QStringLiteral("Save active playback as playlist"),
                          QStringLiteral("Playlist name:"), QLineEdit::Normal, QString{}, &accepted)
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
