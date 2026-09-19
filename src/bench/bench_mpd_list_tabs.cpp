// SPDX-License-Identifier: GPL-3.0-only

// ADR-0188: two server-side list kinds with distinct jobs — working tabs
// (client-owned, temporary, freely editable, played like the queue) and
// MPD stored playlists (long-term, server-owned, in the sidebar). This
// file holds the working tabs, the tab-strip grouping, the copy gestures,
// and the migration of legacy documents.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"

#include "quick/mpd_probe_controller.hpp"
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
#include <QStatusBar>
#include <QTableView>

#include <algorithm>
#include <utility>

namespace trackknife::bench {

namespace {

[[nodiscard]] mpd::Track trackFromListItem(const persistence::ListItem& item) {
    std::vector<mpd::Pair> pairs;
    pairs.reserve(item.fields.size());
    for (const auto& field : item.fields) {
        pairs.push_back(mpd::Pair{
            .name = field.native_name.empty() ? field.name : field.native_name,
            .value = field.value,
        });
    }
    mpd::Track track;
    track.uri = item.source_reference;
    track.metadata = mpd::Metadata{std::move(pairs)};
    if (item.duration_ms) {
        track.duration = std::chrono::milliseconds{*item.duration_ms};
    }
    return track;
}

} // namespace


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
            mpdSearchTabForWidget(qobject_cast<QTableView*>(widget)) == nullptr &&
            mpdListTabForWidget(widget) == nullptr) {
            break;
        }
        ++index;
    }
    return index;
}

BenchMainWindow::MpdListTab* BenchMainWindow::mpdListTabForWidget(QWidget* widget) const {
    const auto found =
        std::ranges::find(mpd_list_tabs_, widget,
                          [](const std::unique_ptr<MpdListTab>& tab) { return tab->view; });
    return found == mpd_list_tabs_.end() ? nullptr : found->get();
}

BenchMainWindow::MpdListTab* BenchMainWindow::currentMpdListTab() const {
    return mpdListTabForWidget(tabs_->currentWidget());
}

void BenchMainWindow::refreshMpdListTabChrome(MpdListTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    const auto name = displayText(tab.document.name);
    tabs_->setTabText(index, name + (tab.document.dirty ? QStringLiteral(" *") : QString{}));
    tabs_->setTabToolTip(
        index, QStringLiteral("Working tab of server tracks — edit freely, play like the "
                              "queue; save to a stored playlist to keep it%1%2")
                   .arg(tab.document.pinned ? QStringLiteral(" · pinned") : QString{},
                        tab.document.dirty ? QStringLiteral(" · modified") : QString{}));
    tab.view->setAccessibleName(QStringLiteral("%1 server track list").arg(name));
    if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!tab.document.pinned);
    }
}

void BenchMainWindow::markMpdListTabDirty(MpdListTab& tab) {
    tab.document.dirty = true;
    refreshMpdListTabChrome(tab);
    schedulePersist();
}

BenchMainWindow::MpdListTab* BenchMainWindow::addMpdListTab(persistence::ListDocument document,
                                                            const bool select) {
    auto tab = std::make_unique<MpdListTab>();
    tab->document = std::move(document);
    tab->model = new quick::MpdQueueModel(this);
    tab->model->setArtworkEnabled(true);
    connect(tab->model, &quick::MpdQueueModel::artworkRequested, mpd_controller_,
            &quick::MpdProbeController::loadServerLibraryArtwork);

    auto* view = new ui::QueueTableView(tabs_);
    tab->view = view;
    const auto id = QString::fromStdString(tab->document.id.to_string());
    view->setObjectName(QStringLiteral("bench-mpd-list-%1").arg(id.left(8)));
    view->setProperty("bench-document-id", id);
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
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    view->setDragEnabled(true);
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setDefaultDropAction(Qt::MoveAction);

    auto* raw_tab = tab.get();
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    // Enter and double-click play like a local list: the whole list replaces
    // the queue and playback starts at the activated row. Appending stays a
    // menu action.
    const auto play_from_row = [this, raw_tab](const QModelIndex& index) {
        if (!index.isValid() || !mpd_controller_->connected()) {
            return;
        }
        QStringList uris;
        const auto tracks = raw_tab->model->tracksSnapshot();
        uris.reserve(static_cast<qsizetype>(tracks.size()));
        for (const auto& track : tracks) {
            uris.push_back(displayText(track.uri));
        }
        if (uris.isEmpty()) {
            return;
        }
        // ADR-0188: playing a working tab stashes the queue instead of
        // destroying it, so the tab behaves like any other playable list.
        if (mpd_controller_->supportsPlaybackContexts()) {
            mpd_controller_->playTrackListContext(uris, index.row());
            return;
        }
        mpd_controller_->replaceQueueWithUrisAndPlayAt(uris, index.row());
    };
    view->setActivateCallback(play_from_row);
    connect(view, &QTableView::doubleClicked, this, play_from_row);
    view->setReorderCallback([this, raw_tab](const QVariantList& rows, int insertion_row) {
        QList<int> moved;
        moved.reserve(rows.size());
        for (const auto& value : rows) {
            moved.push_back(value.toInt());
        }
        const auto first = raw_tab->model->moveTrackRows(moved, insertion_row);
        if (first >= 0) {
            markMpdListTabDirty(*raw_tab);
            raw_tab->view->selectRow(first);
        }
    });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(tab->model, &QAbstractItemModel::modelReset, this,
            [this] { refreshSelectionStatus(); });

    tab->view_layout = mpd_view_layout_;
    const auto binding = QStringLiteral("mpd-list:%1").arg(id);
    if (const auto stored = restored_track_view_layouts_.value(binding); !stored.isEmpty()) {
        if (auto decoded =
                ui::deserializeTrackViewLayout(stored, trackColumnIds(), nullptr)) {
            tab->view_layout = std::move(*decoded);
        }
    }
    applyTrackViewLayout(view, tab->view_layout, mpd_view_layout_);

    std::vector<mpd::Track> tracks;
    tracks.reserve(tab->document.items.size());
    for (const auto& item : tab->document.items) {
        tracks.push_back(trackFromListItem(item));
    }
    tab->model->replaceTracks(std::move(tracks));

    const auto index = tabs_->insertTab(mpdTabInsertionIndex(),
                                        view, QIcon::fromTheme(QStringLiteral("network-server")),
                                        displayText(tab->document.name));
    mpd_list_tabs_.push_back(std::move(tab));
    auto* stored = mpd_list_tabs_.back().get();
    refreshMpdListTabChrome(*stored);
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    refreshTabActions();
    return stored;
}

void BenchMainWindow::closeMpdListTab(MpdListTab* tab) {
    if (tab == nullptr) {
        return;
    }
    const auto index = tabs_->indexOf(tab->view);
    if (index >= 0) {
        tabs_->removeTab(index);
    }
    tab->view->deleteLater();
    tab->model->deleteLater();
    std::erase_if(mpd_list_tabs_,
                  [tab](const std::unique_ptr<MpdListTab>& owned) { return owned.get() == tab; });
    schedulePersist();
    refreshTabActions();
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

BenchMainWindow::MpdListTab*
BenchMainWindow::createServerListTab(const QString& name, std::vector<mpd::Track> tracks) {
    persistence::ListDocument document{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::mpd,
        .name = name.toStdString(),
        .pinned = false,
        .dirty = true,
        .items = {},
    };
    auto* tab = addMpdListTab(std::move(document), true);
    tab->model->appendTracks(std::move(tracks));
    markMpdListTabDirty(*tab);
    return tab;
}




// ADR-0188: send the selection to a working tab — the temporary,
// client-owned list you build up and play like the queue. Long-term
// curation goes to a stored playlist instead (addCopyToServerListMenu).
void BenchMainWindow::addCopyToWorkingTabMenu(QMenu* menu, QTableView* source_view) {
    auto* submenu = menu->addMenu(QStringLiteral("Copy to tab"));
    submenu->setObjectName(QStringLiteral("bench-copy-to-working-tab-menu"));
    const auto has_selection = source_view != nullptr &&
                               source_view->selectionModel() != nullptr &&
                               !source_view->selectionModel()->selectedRows().isEmpty();
    auto* create = submenu->addAction(QStringLiteral("New tab…"));
    create->setObjectName(QStringLiteral("action-copy-to-new-working-tab"));
    create->setEnabled(has_selection);
    connect(create, &QAction::triggered, this, [this, source_view] {
        auto tracks = selectedMpdViewTracks(source_view);
        if (tracks.empty()) {
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("New tab"),
                                                QStringLiteral("Tab name:"), QLineEdit::Normal,
                                                QStringLiteral("Server tracks"), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        createServerListTab(name, std::move(tracks));
    });
    if (!mpd_list_tabs_.empty()) {
        submenu->addSeparator();
        for (const auto& target : mpd_list_tabs_) {
            auto* raw_target = target.get();
            auto* action = submenu->addAction(displayText(target->document.name));
            action->setEnabled(has_selection);
            connect(action, &QAction::triggered, this, [this, source_view, raw_target] {
                if (!std::ranges::any_of(mpd_list_tabs_, [raw_target](const auto& owned) {
                        return owned.get() == raw_target;
                    })) {
                    return;
                }
                auto tracks = selectedMpdViewTracks(source_view);
                if (tracks.empty()) {
                    return;
                }
                raw_target->model->appendTracks(std::move(tracks));
                markMpdListTabDirty(*raw_target);
            });
        }
    }
}

// ADR-0188: one destination per concept — "Copy to tab" builds a working
// tab, this builds or extends a stored playlist. New playlist… creates it
// server-side and opens its tab.
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
    if (mpd_playlists_list_ != nullptr && mpd_playlists_list_->count() > 0) {
        submenu->addSeparator();
        for (int row = 0; row < mpd_playlists_list_->count(); ++row) {
            const auto name = mpd_playlists_list_->item(row)->text();
            auto* action = submenu->addAction(name);
            action->setEnabled(ready);
            connect(action, &QAction::triggered, this,
                    [this, name, uris] { mpd_controller_->addToStoredPlaylist(name, uris, -1); });
        }
    }
}

void BenchMainWindow::showMpdListTrackMenu(MpdListTab& tab, const QPoint& position) {
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
    auto* raw_tab = &tab;

    track_context_menu_->clear();
    auto* replace = track_context_menu_->addAction(QStringLiteral("Replace queue and play"));
    replace->setObjectName(QStringLiteral("action-mpd-list-replace-selection"));
    replace->setEnabled(command_ready && !uris.isEmpty());
    connect(replace, &QAction::triggered, this,
            [this, uris] { mpd_controller_->replaceQueueWithUris(uris); });
    auto* append = track_context_menu_->addAction(QStringLiteral("Append to live queue"));
    append->setObjectName(QStringLiteral("action-mpd-list-append-selection"));
    append->setEnabled(command_ready && !uris.isEmpty());
    connect(append, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, false); });
    auto* next = track_context_menu_->addAction(QStringLiteral("Insert next in live queue"));
    next->setObjectName(QStringLiteral("action-mpd-list-next-selection"));
    next->setEnabled(command_ready && !uris.isEmpty());
    connect(next, &QAction::triggered, this,
            [this, uris] { mpd_controller_->addUris(uris, true); });
    track_context_menu_->addSeparator();
    addMappedLocalTrackActions(track_context_menu_, uris, QStringLiteral("action-mpd-list-"));
    track_context_menu_->addSeparator();
    auto* remove = track_context_menu_->addAction(QStringLiteral("Remove from list"));
    remove->setObjectName(QStringLiteral("action-mpd-list-remove-selection"));
    const auto selected_rows = tab.view->selectionModel() != nullptr
                                   ? tab.view->selectionModel()->selectedRows()
                                   : QModelIndexList{};
    remove->setEnabled(!selected_rows.isEmpty());
    connect(remove, &QAction::triggered, this, [this, raw_tab] {
        if (!std::ranges::any_of(mpd_list_tabs_, [raw_tab](const auto& owned) {
                return owned.get() == raw_tab;
            })) {
            return;
        }
        QList<int> rows;
        for (const auto& index : raw_tab->view->selectionModel()->selectedRows()) {
            rows.push_back(index.row());
        }
        if (!rows.isEmpty()) {
            raw_tab->model->removeTrackRows(std::move(rows));
            markMpdListTabDirty(*raw_tab);
        }
    });
    addCopyToWorkingTabMenu(track_context_menu_, tab.view);
    addCopyToServerListMenu(track_context_menu_, tab.view);
    track_context_menu_->popup(tab.view->viewport()->mapToGlobal(position));
}


} // namespace trackknife::bench
