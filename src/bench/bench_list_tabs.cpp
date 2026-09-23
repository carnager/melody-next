// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/search_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "uicommon/debug_log.hpp"
#include "uicommon/local_files_mime_data.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/rating_stars.hpp"
#include "uicommon/track_row_roles.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr int persist_debounce_ms = 1'000;

} // namespace

void BenchMainWindow::initializePersistence() {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    database_path_ = std::filesystem::path{utf8Bytes(base + QStringLiteral("/lists.sqlite"))};
    // Built once, before anything that needs a catalogue: the panel, the
    // search dialog and dynamic playlists all take this rather than a path.
    catalogue_source_ =
        std::make_unique<CatalogueSource>(database_path_, CatalogueSource::Role::local);
    // Its own connection: the engine serves one connection in order, so a
    // transport command behind a library query would wait for it.
    local_playback_ = new EnginePlayback(*catalogue_source_, this);
    transport_ = local_playback_;
    connect(local_playback_, &EnginePlayback::changed, this, [this] {
        if (transport_ == local_playback_) {
            refreshTransport();
        }
    });
    connect(local_playback_, &EnginePlayback::connected, this, [this] {
        if (transport_ != local_playback_) {
            return;
        }
        // A reconnection is a new engine as far as it is concerned: it knows
        // none of this window's settings, and it may already be playing.
        applyLocalPlaybackModes();
        reattachToEngine();
    });
    if (local_playback_->active()) {
        // An engine starts with its own defaults and has never heard of this
        // window's settings, so they are handed over the moment the connection
        // exists. This runs after the transport is built, which is why sending
        // them there reached nothing and the first track played with no gain
        // applied until a mode was toggled.
        applyLocalPlaybackModes();
    }
    persistence_ = new ui::ListPersistenceService(database_path_, this);
    persistence_timer_ = new QTimer(this);
    persistence_timer_->setSingleShot(true);
    persistence_timer_->setInterval(persist_debounce_ms);
    connect(persistence_timer_, &QTimer::timeout, this, [this] {
        persistNow(false);
        refreshLocalRatings();
    });
    persistence_->initialize([this](ui::PersistedWorkspace workspace, QString error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List restore failed: %1").arg(error), 5'000);
        }
        restored_track_view_layouts_.clear();
        for (const auto& preset : workspace.view_presets) {
            restored_track_view_layouts_.insert(
                displayText(preset.binding),
                QByteArray{preset.header_state.data(),
                           static_cast<qsizetype>(preset.header_state.size())});
        }
        restoreLists(std::move(workspace.lists));
        restoreUpNext();
        // After the lists, because the entry the engine names is looked for in
        // them before a tab is invented for it.
        reattachToEngine();
        // ADR-0227: the remote engine too, after the lists for the same
        // reason -- its tab may already be among them.
        connectRemoteEngine();
        if (error.isEmpty()) {
            local_library_ = new LocalLibraryPanel(*catalogue_source_, source_stack_);
            connect(local_library_, &LocalLibraryPanel::manageFoldersRequested, this,
                    [this] { showSettingsDialog(SettingsDialog::Page::library); });
            source_stack_->addWidget(local_library_);
            connect(
                local_library_, &LocalLibraryPanel::actionRequested, this,
                [this](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
                    if (action == LocalLibraryAction::request_next ||
                        action == LocalLibraryAction::request_end) {
                        local_library_->resolveEntries(
                            std::move(entries), [this, action](std::vector<std::string> paths) {
                                std::vector<LocalTrackRow> rows;
                                for (auto& path : paths) {
                                    LocalTrackRow row;
                                    row.raw_path = std::move(path);
                                    row.title = core::escape_raw_path(
                                        row.raw_path.substr(row.raw_path.find_last_of('/') + 1));
                                    rows.push_back(std::move(row));
                                }
                                enqueueLocalRequests(
                                    std::move(rows),
                                    action == LocalLibraryAction::request_next ? 0 : -1);
                            });
                        return;
                    }
                    auto* target = currentListTab();
                    if (!target || entries.empty()) {
                        return;
                    }
                    const auto id = QString::fromStdString(target->document.id.to_string());
                    const auto name =
                        entries.size() == 1U ? entries.front().label : "Library selection";
                    int insertion = -1;
                    if (action == LocalLibraryAction::next) {
                        insertion = document_text(playback_.anchors.document) == id
                                        ? playback_.row + 1
                                    : target->view->currentIndex().isValid()
                                        ? target->view->currentIndex().row() + 1
                                        : 0;
                    }
                    const QPersistentModelIndex anchor{target->model->index(insertion, 0)};
                    const bool anchored = anchor.isValid();
                    local_library_->resolveEntries(
                        std::move(entries), [this, id, name, action, insertion, anchor,
                                             anchored](std::vector<std::string> paths) {
                            auto* destination = tabForDocument(id);
                            if (!destination || (anchored && !anchor.isValid())) {
                                return;
                            }
                            if (discovery_running_) {
                                statusBar()->showMessage(
                                    QStringLiteral("A file intake is already running"), 3'000);
                                return;
                            }
                            if (action == LocalLibraryAction::new_list) {
                                destination = addListTab(
                                    persistence::ListDocument{.id = core::StableId::random(),
                                                              .kind =
                                                                  persistence::ListKind::scratch,
                                                              .name = name,
                                                              .pinned = false,
                                                              .dirty = false,
                                                              .items = {}},
                                    true);
                                schedulePersist();
                            }
                            startDiscovery(
                                std::move(paths),
                                QString::fromStdString(destination->document.id.to_string()),
                                anchored ? anchor.row() : insertion,
                                action == LocalLibraryAction::replace);
                        });
                });
            connect(local_library_, &LocalLibraryPanel::ratingsChanged, this,
                    &BenchMainWindow::refreshLocalRatings);
            // Restored rows carry their identity hashes; load stored values
            // once the rating store is reachable.
            refreshLocalRatings();
            // ADR-0140: Enter in the library search keeps the full result
            // set as an ordinary scratch list tab.
            connect(local_library_, &LocalLibraryPanel::searchCommitted, this,
                    [this](const QString& query, std::vector<LocalTrackRow> rows) {
                        auto* destination = addListTab(
                            persistence::ListDocument{
                                .id = core::StableId::random(),
                                .kind = persistence::ListKind::scratch,
                                .name = utf8Bytes(QStringLiteral("Search: %1").arg(query)),
                                .pinned = false,
                                .dirty = false,
                                .items = {},
                            },
                            true);
                        destination->model->appendRows(std::move(rows));
                        markTabDirty(*destination);
                        syncArtwork(*destination);
                    });
            refreshActiveContext();
        }
        startMetadataOperationRecovery();
    });
}

void BenchMainWindow::restoreLists(std::vector<persistence::ListDocument> documents) {
    lists_restored_ = true;
    qCDebug(tkDebug) << "restoring" << static_cast<int>(documents.size()) << "list documents";
    for (auto& document : documents) {
        qCDebug(tkDebug) << "  list" << displayText(document.name) << "kind"
                         << static_cast<int>(document.kind) << "items"
                         << static_cast<int>(document.items.size());
        // Documents of the retired MPD backend name server URIs, not files;
        // opened as local lists they would be rows that cannot play.
        if (document.kind == persistence::ListKind::mpd) {
            continue;
        }
        addListTab(std::move(document), false);
    }
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    } else {
        tabs_->setCurrentWidget(list_tabs_.front()->view);
    }
    if (!pending_open_paths_.empty()) {
        auto pending = std::exchange(pending_open_paths_, std::vector<std::string>{});
        openLocalPaths(std::move(pending));
    }
}

void BenchMainWindow::schedulePersist() {
    if (persistence_timer_ != nullptr) {
        persistence_timer_->start();
    }
}

std::vector<persistence::ListDocument> BenchMainWindow::collectDocuments() {
    std::vector<persistence::ListDocument> documents;
    documents.reserve(static_cast<std::size_t>(tabs_->count()));
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view == nullptr) {
            continue;
        }
        const auto id = view->property("bench-document-id").toString();
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            continue;
        }
        auto document = tab->document;
        document.items.clear();
        document.items.reserve(tab->model->rows().size());
        for (const auto& row : tab->model->rows()) {
            persistence::ListItem item{
                // Carry the row's identity rather than letting ListItem mint a
                // fresh one, which would reassign every entry on every save.
                .entry_id = row.entry_id,
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = row.raw_path,
                .logical_reference = row.logical_reference,
                .segment = row.segment ? std::optional{persistence::ListItemSegment{
                                             .start_sample = row.segment->start_sample,
                                             .end_sample = row.segment->end_sample,
                                         }}
                                       : std::nullopt,
                .source_selection = row.selection.stream_index || row.selection.subsong_index
                                        ? std::optional{persistence::ListItemSourceSelection{
                                              .audio_stream_index = row.selection.stream_index,
                                              .subsong_index = row.selection.subsong_index,
                                          }}
                                        : std::nullopt,
                .duration_ms = row.duration_ms,
                .source_revision = row.source_revision,
                .fields = {},
            };
            if (!row.metadata.fields.empty()) {
                // This remains a presentation cache, but retaining layers is
                // necessary so a verified embedded refresh cannot erase CUE,
                // chapter, or sidecar projections for the same physical file.
                for (const auto& field : row.metadata.fields) {
                    if (field.canonical_name.empty()) {
                        continue;
                    }
                    for (const auto& value : field.values) {
                        item.fields.push_back({
                            .name = field.canonical_name,
                            .value = value,
                            .native_name = field.native_name,
                            .provenance = field.provenance,
                            .language = field.qualifier.language,
                            .description = field.qualifier.description,
                        });
                    }
                }
            } else {
                if (!row.title.empty()) {
                    item.fields.push_back({.name = "title", .value = row.title});
                }
                if (!row.artist.empty()) {
                    item.fields.push_back({.name = "artist", .value = row.artist});
                }
                if (!row.album.empty()) {
                    item.fields.push_back({.name = "album", .value = row.album});
                }
                if (!row.album_artist.empty()) {
                    item.fields.push_back({.name = "albumartist", .value = row.album_artist});
                }
                if (!row.date.empty()) {
                    item.fields.push_back({.name = "date", .value = row.date});
                }
                if (!row.track_number.empty()) {
                    item.fields.push_back({.name = "track", .value = row.track_number});
                }
            }
            document.items.push_back(std::move(item));
        }
        documents.push_back(std::move(document));
    }
    return documents;
}

std::vector<persistence::TrackViewPreset> BenchMainWindow::collectTrackViewLayouts() {
    std::vector<persistence::TrackViewPreset> layouts;
    layouts.reserve(list_tabs_.size());
    for (const auto& tab : list_tabs_) {
        const auto id = QString::fromStdString(tab->document.id.to_string());
        const auto bytes = tab->view_layout_persistence_protected
                               ? tab->preserved_view_layout
                               : ui::serializeTrackViewLayout(captureTrackViewLayout(*tab));
        layouts.push_back(persistence::TrackViewPreset{
            .binding = utf8Bytes(QStringLiteral("local:%1").arg(id)),
            .header_state = std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())},
        });
    }
    return layouts;
}

void BenchMainWindow::persistNow(const bool wait) {
    if (persistence_ == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    auto view_layouts = collectTrackViewLayouts();
    if (wait) {
        const auto error =
            persistence_->saveWorkspaceAndWait(std::move(documents), std::move(view_layouts));
        if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
        }
        return;
    }
    persistence_->saveWorkspace(
        std::move(documents), std::move(view_layouts), [this](QString error) {
            if (!error.isEmpty()) {
                statusBar()->showMessage(QStringLiteral("List save failed: %1").arg(error), 5'000);
            }
        });
}

void BenchMainWindow::backupWorkspace() {
    if (persistence_ == nullptr) {
        return;
    }
    auto* dialog = new QFileDialog(this, tr("Back up Trackknife workspace database"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setDefaultSuffix(QStringLiteral("sqlite"));
    dialog->setNameFilter(tr("Trackknife workspace database (*.sqlite)"));
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->selectFile(QStringLiteral("trackknife-workspace.sqlite"));
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        const auto settings_path = path + QStringLiteral(".settings.ini");
        if (QFile::exists(settings_path)) {
            statusBar()->showMessage(
                QStringLiteral("Workspace backup failed: %1 already exists").arg(settings_path),
                10'000);
            return;
        }
        const auto temporary_settings = settings_path + QStringLiteral(".partial");
        if (QFile::exists(temporary_settings)) {
            statusBar()->showMessage(
                QStringLiteral("Workspace backup failed: stale temporary settings file exists"),
                10'000);
            return;
        }
        QSettings current;
        QSettings settings_backup{temporary_settings, QSettings::IniFormat};
        settings_backup.setValue(QStringLiteral("backup/format"), 1);
        for (const auto& key : current.allKeys()) {
            settings_backup.setValue(QStringLiteral("values/") + key, current.value(key));
        }
        settings_backup.sync();
        if (settings_backup.status() != QSettings::NoError) {
            QFile::remove(temporary_settings);
            statusBar()->showMessage(QStringLiteral("Workspace settings backup failed"), 10'000);
            return;
        }
        const auto encoded = QFile::encodeName(path);
        const auto destination = std::filesystem::path{
            std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())}};
        persistNow(false);
        statusBar()->showMessage(QStringLiteral("Backing up workspace database…"));
        persistence_->backupDatabase(destination, [this, path, settings_path,
                                                   temporary_settings](QString error) {
            if (error.isEmpty() && !QFile::rename(temporary_settings, settings_path)) {
                error = QStringLiteral("database saved, but settings could not be published");
            } else if (!error.isEmpty()) {
                QFile::remove(temporary_settings);
            }
            statusBar()->showMessage(
                error.isEmpty()
                    ? QStringLiteral("Workspace backed up to %1 and %2").arg(path, settings_path)
                    : QStringLiteral("Workspace backup failed: %1").arg(error),
                error.isEmpty() ? 7'000 : 10'000);
        });
    });
    dialog->open();
}

void BenchMainWindow::scheduleWorkspaceRestore() {
    const auto path =
        QFileDialog::getOpenFileName(this, tr("Restore Trackknife workspace database"), {},
                                     tr("Trackknife workspace database (*.sqlite);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const auto answer = QMessageBox::warning(
        this, tr("Restore workspace database"),
        tr("Trackknife will validate and restore this database at the next start. "
           "The current database will be retained beside it for rollback. Close Trackknife now?"),
        QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Close) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("recovery/pending-workspace-restore"), path);
    const auto settings_backup = path + QStringLiteral(".settings.ini");
    settings.setValue(QStringLiteral("recovery/pending-settings-restore"),
                      QFile::exists(settings_backup) ? settings_backup : QString{});
    settings.sync();
    close();
}

BenchMainWindow::ListTab* BenchMainWindow::addListTab(persistence::ListDocument document,
                                                      const bool select) {
    const auto id = QString::fromStdString(document.id.to_string());
    auto* model = new LocalListModel(tabs_);
    std::vector<LocalTrackRow> restored_rows;
    restored_rows.reserve(document.items.size());
    for (const auto& item : document.items) {
        if (item.source != persistence::ListSource::local) {
            continue;
        }
        LocalTrackRow row;
        row.entry_id = item.entry_id;
        row.raw_path = item.source_reference;
        row.logical_reference = item.logical_reference;
        if (item.source_selection) {
            row.selection = formats::AudioSourceSelection{
                .stream_index = item.source_selection->audio_stream_index,
                .subsong_index = item.source_selection->subsong_index,
            };
        }
        if (item.segment) {
            row.segment = formats::SampleRange{.start_sample = item.segment->start_sample,
                                               .end_sample = item.segment->end_sample};
        }
        row.duration_ms = item.duration_ms;
        row.source_revision = item.source_revision;
        for (const auto& field : item.fields) {
            const auto canonical_name =
                field.name.empty()
                    ? metadata::resolve_text_property_identity(field.native_name).canonical_name
                    : field.name;
            if (!canonical_name.empty()) {
                row.metadata.fields.push_back(metadata::MetadataField{
                    .canonical_name = canonical_name,
                    .native_name = field.native_name.empty() ? field.name : field.native_name,
                    .values = {field.value},
                    .qualifier =
                        metadata::FieldQualifier{
                            .language = field.language,
                            .description = field.description,
                        },
                    .provenance = field.provenance,
                });
            }
        }
        remove_shadowed_probed_metadata(row.metadata);
        project_display_metadata(row);
        row.probed = row.selection.stream_index.has_value() ||
                     row.selection.subsong_index.has_value() || row.segment.has_value() ||
                     row.duration_ms.has_value() || !item.fields.empty();
        restored_rows.push_back(std::move(row));
    }
    model->replaceRows(std::move(restored_rows));
    model->setListeningHistoryService(persistence_);

    auto* view = new ui::QueueTableView(tabs_);
    view->setObjectName(QStringLiteral("bench-list-%1").arg(id.left(8)));
    view->setProperty("bench-document-id", id);
    view->setModel(model);
    connect(
        model, &LocalListModel::historyRowsRestored, view, [view, model](const QList<int>& rows) {
            QItemSelection selection;
            for (const auto row : rows)
                selection.select(model->index(row, 0), model->index(row, model->columnCount() - 1));
            view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
            if (!rows.empty())
                view->selectionModel()->setCurrentIndex(model->index(rows.front(), 0),
                                                        QItemSelectionModel::NoUpdate);
        });
    connect(model, &QAbstractItemModel::rowsInserted, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &QAbstractItemModel::rowsRemoved, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &QAbstractItemModel::modelReset, this,
            &BenchMainWindow::refreshListHistoryActions);
    view->addAction(undo_list_action_);
    view->addAction(redo_list_action_);
    connect(model, &LocalListModel::historyChanged, this,
            &BenchMainWindow::refreshListHistoryActions);
    connect(model, &LocalListModel::historyDiscarded, this,
            [this](const QString& reason) { statusBar()->showMessage(reason, 5'000); });
    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& first) {
        if (first.column() < local_play_count_column)
            refreshSelectionStatus();
    });
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshSelectionStatus(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { refreshSelectionStatus(); });
    view->setProperty("trackknife-hover-row", -1);
    view->setAlternatingRowColors(true);
    view->setShowGrid(false);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setWordWrap(false);
    view->setTextElideMode(Qt::ElideRight);
    view->verticalHeader()->setDefaultSectionSize(22);
    view->verticalHeader()->setMinimumSectionSize(18);
    view->verticalHeader()->hide();
    view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    view->horizontalHeader()->setSectionsMovable(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->horizontalHeader()->setMinimumSectionSize(24);
    view->horizontalHeader()->setMaximumSectionSize(4'096);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view->horizontalHeader(), &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackViewHeaderMenu(view, position); });
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& position) { showTrackContextMenu(view, position); });
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropOverwriteMode(false);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setActivateCallback([this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    connect(view, &QTableView::doubleClicked, this, [this, id](const QModelIndex& index) {
        auto* tab = tabForDocument(id);
        if (tab != nullptr && index.isValid()) {
            playRow(*tab, index.row());
        }
    });
    view->setReorderCallback([this, id](const QVariantList& rows, const int insertion_row) {
        auto* tab = tabForDocument(id);
        if (tab == nullptr) {
            return;
        }
        std::vector<int> row_indexes;
        row_indexes.reserve(static_cast<std::size_t>(rows.size()));
        for (const auto& row : rows) {
            row_indexes.push_back(row.toInt());
        }
        tab->model->reorderRows(std::move(row_indexes), insertion_row);
        markTabDirty(*tab);
    });
    view->setExternalDropCallback([this, id](QAbstractItemView* source, const QVariantList& rows,
                                             const int insertion_row, const Qt::DropAction action) {
        auto* table = qobject_cast<QTableView*>(source);
        return table != nullptr &&
               transferRows(table, rows, id, action == Qt::MoveAction, insertion_row);
    });
    view->setLocalFilesDropCallback(
        [this, id](const ui::LocalFilesMimeData& files, int insertion_row) {
            auto* target = tabForDocument(id);
            if (!target || discovery_running_) {
                return false;
            }
            const QPersistentModelIndex anchor{target->model->index(insertion_row, 0)};
            const bool anchored = anchor.isValid();
            const QPointer<BenchMainWindow> window{this};
            files.resolve(
                [window, id, insertion_row, anchor, anchored](std::vector<std::string> paths) {
                    if (window && window->tabForDocument(id) && (!anchored || anchor.isValid())) {
                        window->startDiscovery(std::move(paths), id,
                                               anchored ? anchor.row() : insertion_row);
                    }
                });
            return true;
        });
    view->setLocalUrlDropCallback([this, id](const QList<QUrl>& urls, const int insertion_row) {
        std::vector<std::string> raw_paths;
        raw_paths.reserve(static_cast<std::size_t>(urls.size()));
        for (const auto& url : urls) {
            if (!url.isLocalFile()) {
                continue;
            }
            const auto encoded = QFile::encodeName(url.toLocalFile());
            raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
        if (raw_paths.empty()) {
            return false;
        }
        startDiscovery(std::move(raw_paths), id, insertion_row);
        return true;
    });

    const auto index = tabs_->addTab(view, displayText(document.name));
    if (document.remote) {
        // ADR-0227: which engine a tab plays on is visible, not remembered.
        tabs_->setTabIcon(index, QIcon::fromTheme(QStringLiteral("network-server")));
        tabs_->setTabToolTip(index, tr("Plays on the remote engine"));
    }
    auto tab = std::make_unique<ListTab>();
    tab->document = std::move(document);
    tab->model = model;
    tab->view = view;
    auto* raw_tab = tab.get();
    list_tabs_.push_back(std::move(tab));
    view->setProperty("bench-tab-pointer", QVariant::fromValue<void*>(raw_tab));
    auto layout = defaultTrackViewLayout();
    const auto binding = QStringLiteral("local:%1").arg(id);
    if (const auto stored = restored_track_view_layouts_.value(binding); !stored.isEmpty()) {
        QString layout_error;
        if (auto decoded = ui::deserializeTrackViewLayout(stored, trackColumnIds(), &layout_error);
            decoded) {
            layout = std::move(*decoded);
        } else {
            raw_tab->view_layout_persistence_protected = true;
            raw_tab->preserved_view_layout = stored;
            statusBar()->showMessage(
                QStringLiteral("Track layout was not loaded (%1); the saved value was preserved")
                    .arg(layout_error),
                7'000);
        }
    }
    applyTrackViewLayout(*raw_tab, layout);
    connect(view->horizontalHeader(), &QHeaderView::sectionMoved, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* moved_tab = tabForDocument(id);
                if (moved_tab == nullptr) {
                    return;
                }
                moved_tab->view_layout = captureTrackViewLayout(*moved_tab);
                moved_tab->view_layout_persistence_protected = false;
                moved_tab->preserved_view_layout.clear();
                schedulePersist();
                refreshTrackViewActions();
            });
    connect(view->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this, id](const int, const int, const int) {
                if (applying_track_view_layout_) {
                    return;
                }
                auto* resized_tab = tabForDocument(id);
                if (resized_tab == nullptr) {
                    return;
                }
                resized_tab->view_layout = captureTrackViewLayout(*resized_tab);
                resized_tab->view_layout_persistence_protected = false;
                resized_tab->preserved_view_layout.clear();
                schedulePersist();
            });
    refreshTabChrome(*raw_tab);
    if (select) {
        tabs_->setCurrentIndex(index);
        view->setFocus(Qt::ShortcutFocusReason);
    }
    enqueueUnprobedRows(*raw_tab);
    syncArtwork(*raw_tab);
    refreshTabActions();
    refreshSelectionStatus();
    return raw_tab;
}

void BenchMainWindow::openSearchDialog() {
    if (search_dialog_ != nullptr) {
        search_dialog_->show();
        search_dialog_->raise();
        search_dialog_->activateWindow();
        search_dialog_->focusInput();
        return;
    }
    // ADR-0153: database scope reads the workspace index; tab scope
    // snapshots the current local tab's rows and reports on-demand
    // technicals back onto every tab holding the probed file.
    search_dialog_ = new SearchDialog(
        *catalogue_source_,
        [this]() -> std::optional<SearchDialog::TabSnapshot> {
            auto* tab = currentListTab();
            if (tab == nullptr) {
                return std::nullopt;
            }
            return SearchDialog::TabSnapshot{QString::fromUtf8(tab->document.name),
                                             tab->model->rows()};
        },
        [this](std::string raw_path, LocalTrackTechnicals technicals) {
            for (const auto& tab : list_tabs_) {
                tab->model->applyTechnicals(raw_path, technicals);
            }
        },
        this);
    search_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    const auto watch_tab = [this, dialog = search_dialog_] {
        if (!dialog)
            return;
        auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
        dialog->watchCurrentModel(view ? view->model() : nullptr);
    };
    connect(tabs_, &QTabWidget::currentChanged, search_dialog_, watch_tab);
    watch_tab();
    connect(search_dialog_, &SearchDialog::rowsRequested, this,
            [this](const QString& name, std::vector<LocalTrackRow> rows,
                   const LocalLibraryAction action) {
                auto* destination = currentListTab();
                int insertion = -1;
                if (action == LocalLibraryAction::new_list) {
                    destination =
                        addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                             .kind = persistence::ListKind::scratch,
                                                             .name = utf8Bytes(name),
                                                             .pinned = false,
                                                             .dirty = false,
                                                             .items = {}},
                                   true);
                    schedulePersist();
                } else if (destination != nullptr && action == LocalLibraryAction::next) {
                    const auto id = QString::fromStdString(destination->document.id.to_string());
                    insertion = document_text(playback_.anchors.document) == id ? playback_.row + 1
                                : destination->view->currentIndex().isValid()
                                    ? destination->view->currentIndex().row() + 1
                                    : 0;
                }
                if (destination == nullptr) {
                    return;
                }
                if (action == LocalLibraryAction::replace) {
                    destination->model->replaceRows(std::move(rows), true);
                } else {
                    destination->model->appendRows(std::move(rows), insertion);
                }
                markTabDirty(*destination);
                syncArtwork(*destination);
                if (action == LocalLibraryAction::replace && destination->model->rowCount() > 0) {
                    playRow(*destination, 0);
                }
            });
    search_dialog_->show();
}

BenchMainWindow::ListTab* BenchMainWindow::currentListTab() {
    auto* view = qobject_cast<QTableView*>(tabs_->currentWidget());
    if (view == nullptr) {
        return nullptr;
    }
    return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
}

void BenchMainWindow::refreshActiveContext() {
    if (source_stack_ != nullptr) {
        const auto index = local_source_tabs_ != nullptr ? local_source_tabs_->currentIndex() : 0;
        const bool remote_view = local_source_tabs_ != nullptr &&
                                 local_source_tabs_->tabData(index) == QStringLiteral("remote");
        auto* source =
            remote_view && remote_library_ != nullptr ? static_cast<QWidget*>(remote_library_)
            : index == 1 && local_library_ != nullptr ? static_cast<QWidget*>(local_library_)
                                                      : static_cast<QWidget*>(folder_view_);
        source_stack_->setCurrentWidget(source);
    }
    if (folder_bookmarks_ != nullptr) {
        const auto folders_visible =
            local_source_tabs_ == nullptr || local_source_tabs_->currentIndex() == 0;
        folder_bookmarks_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
        folder_bookmarks_heading_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
    }
    if (device_menu_ != nullptr && device_menu_->isEmpty()) {
        rebuildDeviceMenu();
    }
    refreshLocalPlaybackControls();
    if (seek_ != nullptr) {
        refreshTransport();
    }
}

BenchMainWindow::ListTab* BenchMainWindow::tabForDocument(const core::StableId& document_id) {
    if (document_id.is_nil()) {
        return nullptr;
    }
    return tabForDocument(QString::fromStdString(document_id.to_string()));
}

BenchMainWindow::ListTab* BenchMainWindow::tabForDocument(const QString& document_id) {
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
        if (view != nullptr && view->property("bench-document-id").toString() == document_id) {
            return static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
        }
    }
    if (detached_playback_ &&
        QString::fromStdString(detached_playback_->document.id.to_string()) == document_id)
        return &*detached_playback_;
    return nullptr;
}

bool BenchMainWindow::transferRows(QTableView* source, const QVariantList& rows,
                                   const QString& target_id, const bool move,
                                   const int insertion_row) {
    auto* target = tabForDocument(target_id);
    if (target == nullptr || source == nullptr) {
        return false;
    }
    auto* source_tab = static_cast<ListTab*>(source->property("bench-tab-pointer").value<void*>());
    auto* source_model = qobject_cast<LocalListModel*>(source->model());
    const bool dynamic = source->property("definition-owned").toBool();
    if (!source_model || source_tab == target || (!source_tab && !dynamic) || (dynamic && move)) {
        return false;
    }
    std::vector<LocalTrackRow> transferred;
    std::vector<int> source_rows;
    transferred.reserve(static_cast<std::size_t>(rows.size()));
    source_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        const auto row_index = row.toInt();
        if (row_index < 0 || row_index >= static_cast<int>(source_model->rows().size())) {
            continue;
        }
        transferred.push_back(source_model->rows()[static_cast<std::size_t>(row_index)]);
        source_rows.push_back(row_index);
    }
    if (transferred.empty()) {
        return false;
    }
    CrossTabMoveEdit coordinated;
    if (move) {
        coordinated.source_id = source->property("bench-document-id").toString();
        coordinated.target_id = target_id;
        coordinated.source_before = source_tab->model->rows();
        coordinated.target_before = target->model->rows();
    }
    target->model->appendRows(std::move(transferred), insertion_row, !move);
    enqueueUnprobedRows(*target);
    markTabDirty(*target);
    syncArtwork(*target);
    if (move) {
        source_tab->model->removeRowIndexes(std::move(source_rows), false);
        markTabDirty(*source_tab);
        coordinated.source_after = source_tab->model->rows();
        coordinated.target_after = target->model->rows();
        cross_tab_move_edit_ = std::move(coordinated);
    }
    refreshListHistoryActions();
    return true;
}

bool BenchMainWindow::canReplayCrossTabMove(const bool undo) {
    if (!cross_tab_move_edit_ || cross_tab_move_edit_->applied != undo) {
        return false;
    }
    const auto* source = tabForDocument(cross_tab_move_edit_->source_id);
    const auto* target = tabForDocument(cross_tab_move_edit_->target_id);
    if (source == nullptr || target == nullptr) {
        return false;
    }
    const auto* current = currentListTab();
    if (current != source && current != target) {
        return false;
    }
    return source->model->rows() ==
               (undo ? cross_tab_move_edit_->source_after : cross_tab_move_edit_->source_before) &&
           target->model->rows() ==
               (undo ? cross_tab_move_edit_->target_after : cross_tab_move_edit_->target_before);
}

bool BenchMainWindow::replayCrossTabMove(const bool undo) {
    if (!canReplayCrossTabMove(undo)) {
        return false;
    }
    auto* source = tabForDocument(cross_tab_move_edit_->source_id);
    auto* target = tabForDocument(cross_tab_move_edit_->target_id);
    source->model->replaceRows(undo ? cross_tab_move_edit_->source_before
                                    : cross_tab_move_edit_->source_after);
    target->model->replaceRows(undo ? cross_tab_move_edit_->target_before
                                    : cross_tab_move_edit_->target_after);
    cross_tab_move_edit_->applied = !undo;
    for (auto* tab : {source, target}) {
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
    }
    refreshSelectionStatus();
    return true;
}

bool BenchMainWindow::transferRowsToNewTab(QTableView* source, const QVariantList& rows,
                                           const bool move, const QString& name) {
    const auto* source_tab = source == nullptr
                                 ? nullptr
                                 : tabForDocument(source->property("bench-document-id").toString());
    auto* source_model = source ? qobject_cast<LocalListModel*>(source->model()) : nullptr;
    const bool dynamic = source && source->property("definition-owned").toBool();
    if (!source_model || (!dynamic && (!source_tab || source_tab->view != source)) ||
        (dynamic && move) || rows.isEmpty() ||
        std::ranges::any_of(rows, [source_model](const QVariant& row) {
            bool valid = false;
            const auto index = row.toInt(&valid);
            return !valid || index < 0 || index >= source_model->rowCount();
        })) {
        return false;
    }
    auto* destination = addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                             .kind = persistence::ListKind::scratch,
                                                             .name = utf8Bytes(name),
                                                             .pinned = false,
                                                             .dirty = false,
                                                             .items = {}},
                                   false);
    const auto transferred = transferRows(
        source, rows, QString::fromStdString(destination->document.id.to_string()), move, -1);
    if (transferred)
        tabs_->setCurrentWidget(destination->view);
    return transferred;
}

void BenchMainWindow::markTabDirty(ListTab& tab) {
    tab.document.dirty = true;
    refreshTabChrome(tab);
    schedulePersist();
    // An edit to the list that is playing is an edit to the engine's queue.
    // Without this the engine keeps playing the list as it was when play was
    // pressed, and a track removed here still plays.
    if (tab.document.id == playback_.anchors.document) {
        syncEngineQueue();
    }
}

void BenchMainWindow::setActiveLocalList(const QString& id) {
    if (active_local_list_id_ == id)
        return;
    active_local_list_id_ = id;
    for (const auto& tab : list_tabs_)
        refreshTabChrome(*tab);
}

void BenchMainWindow::refreshTabChrome(ListTab& tab) {
    const auto index = tabs_->indexOf(tab.view);
    if (index < 0) {
        return;
    }
    const auto name = displayText(tab.document.name);
    const auto active =
        QString::fromStdString(tab.document.id.to_string()) == active_local_list_id_;
    tab.view->setProperty("bench-playback-active", active);
    tabs_->tabBar()->setTabTextColor(index, QColor{});
    tabs_->tabBar()->setTabData(index, active);
    tabs_->setTabText(index, name + (tab.document.dirty ? QStringLiteral(" *") : QString{}));
    tabs_->setTabIcon(index, active ? playbackSpeakerIcon(tabs_->palette()) : QIcon{});
    const auto kind = tab.document.kind == persistence::ListKind::scratch
                          ? QStringLiteral("Persistent scratch list")
                          : QStringLiteral("Named Trackknife working list");
    tabs_->setTabToolTip(index,
                         QStringLiteral("%1%2%3").arg(
                             kind, tab.document.pinned ? QStringLiteral(" · pinned") : QString{},
                             tab.document.dirty ? QStringLiteral(" · modified") : QString{}));
    if (active)
        tabs_->setTabToolTip(index, tabs_->tabToolTip(index) + tr(" · Active playback queue"));
    tab.view->setAccessibleName(QStringLiteral("%1 track list").arg(name));
    if (auto* close = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
        close->setVisible(!tab.document.pinned);
    }
}

void BenchMainWindow::refreshTabActions() {
    refreshListHistoryActions();
    const auto* tab = currentListTab();
    const bool available = tab != nullptr;
    if (export_playlist_action_)
        export_playlist_action_->setEnabled(available);
    if (list_edit_bar_)
        list_edit_bar_->setView(available ? tab->view : nullptr);
    if (list_find_bar_ != nullptr) {
        auto* find_view = available ? tab->view : nullptr;
        list_find_bar_->setView(find_view);
        find_list_action_->setEnabled(find_view != nullptr);
        find_next_action_->setEnabled(find_view != nullptr);
        find_previous_action_->setEnabled(find_view != nullptr);
    }
    for (auto* action :
         {duplicate_tab_action_, pin_tab_action_, save_tab_action_, rename_tab_action_}) {
        if (action != nullptr) {
            action->setEnabled(available);
        }
    }
    if (pin_tab_action_ != nullptr) {
        const QSignalBlocker blocker{pin_tab_action_};
        pin_tab_action_->setChecked(available && tab->document.pinned);
    }
    if (close_tab_action_ != nullptr) {
        const auto properties_tab =
            qobject_cast<MetadataPropertiesDialog*>(tabs_->currentWidget()) != nullptr;
        close_tab_action_->setEnabled(properties_tab || (available && !tab->document.pinned));
    }
}

// Closing returns you to the tab you were on before this one, not to
// whichever tab happens to sit next to it — the neighbour is rarely where
// you came from.
void BenchMainWindow::rememberTabVisit(QWidget* tab) {
    if (tab == nullptr) {
        return;
    }
    tab_visit_history_.removeIf(
        [tab](const QPointer<QWidget>& seen) { return seen == nullptr || seen == tab; });
    tab_visit_history_.push_front(tab);
    constexpr qsizetype remembered_tabs = 32;
    while (tab_visit_history_.size() > remembered_tabs) {
        tab_visit_history_.pop_back();
    }
}

// The tab to fall back to when `closed` goes away: the most recent visit
// that is not it. Removing a tab makes Qt select a neighbour, which pushes
// that neighbour onto the history — so the answer is computed first, before
// the close runs.
QPointer<QWidget> BenchMainWindow::previouslyVisitedTab(QWidget* closed) const {
    for (const auto& candidate : tab_visit_history_) {
        if (candidate != nullptr && candidate != closed && tabs_->indexOf(candidate) >= 0) {
            return candidate;
        }
    }
    return {};
}

void BenchMainWindow::closeTabAt(const int index) {
    // The tab being closed may not be the current one, and the close path
    // runs asynchronously for some kinds, so the restore happens after.
    auto* closing = tabs_->widget(index);
    const QPointer<QWidget> guard{closing};
    if (closing == tabs_->currentWidget()) {
        const auto restore = previouslyVisitedTab(closing);
        // Some close paths ask for confirmation or finish asynchronously, so
        // the restore only applies once the tab is actually gone.
        QTimer::singleShot(0, this, [this, guard, restore] {
            const auto closed = guard == nullptr || tabs_->indexOf(guard) < 0;
            if (closed && restore != nullptr && tabs_->indexOf(restore) >= 0) {
                tabs_->setCurrentWidget(restore);
            }
        });
    }
    if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
        properties->close();
        return;
    }
    auto* view = qobject_cast<QTableView*>(tabs_->widget(index));
    if (view == nullptr) {
        return;
    }
    auto* tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (tab == nullptr || tab->document.pinned) {
        statusBar()->showMessage(QStringLiteral("Unpin this list before closing it"), 3'000);
        return;
    }
    if (tab->document.dirty) {
        QMessageBox confirmation{
            QMessageBox::Question,
            QStringLiteral("Close unsaved list"),
            QStringLiteral("Discard the unsaved contents of “%1”?")
                .arg(displayText(tab->document.name)),
            QMessageBox::Yes | QMessageBox::No,
            this,
        };
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::No);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    tabs_->removeTab(index);
    view->deleteLater();
    if (playback_.requests.active() && tab->document.id == playback_.anchors.document) {
        if (detached_playback_)
            detached_playback_->model->deleteLater();
        detached_playback_ = *tab;
        detached_playback_->view = nullptr;
    } else
        tab->model->deleteLater();
    std::erase_if(list_tabs_,
                  [tab](const std::unique_ptr<ListTab>& owned) { return owned.get() == tab; });
    if (list_tabs_.empty()) {
        addListTab(
            persistence::ListDocument{
                .id = core::StableId::random(),
                .kind = persistence::ListKind::scratch,
                .name = "Local Queue",
                .pinned = false,
                .dirty = false,
                .items = {},
            },
            true);
    }
    schedulePersist();
    refreshTabActions();
}

void BenchMainWindow::closeCurrentTab() { closeTabAt(tabs_->currentIndex()); }

void BenchMainWindow::createList() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("New list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, QString{}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    addListTab(
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = utf8Bytes(name),
            .pinned = false,
            .dirty = false,
            .items = {},
        },
        true);
    schedulePersist();
}

void BenchMainWindow::duplicateCurrentTab() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    auto documents = collectDocuments();
    const auto found =
        std::ranges::find(documents, tab->document.id, &persistence::ListDocument::id);
    if (found == documents.end()) {
        return;
    }
    auto duplicate = *found;
    duplicate.id = core::StableId::random();
    duplicate.name = utf8Bytes(QStringLiteral("%1 copy").arg(displayText(found->name)));
    duplicate.pinned = false;
    duplicate.dirty = true;
    auto* duplicated_tab = addListTab(std::move(duplicate), true);
    if (duplicated_tab != nullptr) {
        applyTrackViewLayout(*duplicated_tab, captureTrackViewLayout(*tab));
    }
    schedulePersist();
}

void BenchMainWindow::toggleCurrentTabPinned() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    tab->document.pinned = !tab->document.pinned;
    refreshTabChrome(*tab);
    refreshTabActions();
    schedulePersist();
}

void BenchMainWindow::saveCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    if (tab->document.kind == persistence::ListKind::scratch) {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Save working list"),
                                                QStringLiteral("Name:"), QLineEdit::Normal,
                                                displayText(tab->document.name), &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty()) {
            return;
        }
        tab->document.name = utf8Bytes(name);
        tab->document.kind = persistence::ListKind::saved;
    }
    tab->document.dirty = false;
    refreshTabChrome(*tab);
    schedulePersist();
}

void BenchMainWindow::renameCurrentList() {
    auto* tab = currentListTab();
    if (tab == nullptr) {
        return;
    }
    const auto current_name = displayText(tab->document.name);
    bool accepted = false;
    const auto name =
        QInputDialog::getText(this, QStringLiteral("Rename list"), QStringLiteral("Name:"),
                              QLineEdit::Normal, current_name, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name == current_name) {
        return;
    }
    tab->document.name = utf8Bytes(name);
    markTabDirty(*tab);
}

void BenchMainWindow::showTabContextMenu(const QPoint& position) {
    const auto index = tabs_->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }
    tabs_->setCurrentIndex(index);
    refreshTabActions();
    tab_context_menu_->popup(tabs_->tabBar()->mapToGlobal(position));
}

void BenchMainWindow::showTrackContextMenu(QTableView* view, const QPoint& position) {
    if (view == nullptr || view->selectionModel() == nullptr || track_context_menu_ == nullptr) {
        return;
    }
    const auto target = view->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    tabs_->setCurrentWidget(view);

    const auto* grouped_delegate = qobject_cast<const ui::QueueItemDelegate*>(view->itemDelegate());
    const auto relative_y = position.y() - view->visualRect(target).top();
    if (grouped_delegate != nullptr && grouped_delegate->isAlbumHeaderHit(target, relative_y)) {
        const auto [first, last] = grouped_delegate->albumRowRange(target);
        const QItemSelection album{view->model()->index(first, 0),
                                   view->model()->index(last, view->model()->columnCount() - 1)};
        view->selectionModel()->select(album, QItemSelectionModel::ClearAndSelect |
                                                  QItemSelectionModel::Rows);
        view->selectionModel()->setCurrentIndex(view->model()->index(first, local_title_column),
                                                QItemSelectionModel::NoUpdate);
    } else {
        if (!view->selectionModel()->isRowSelected(target.row(), target.parent())) {
            view->selectionModel()->select(target, QItemSelectionModel::ClearAndSelect |
                                                       QItemSelectionModel::Rows);
        }
        view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::NoUpdate);
    }
    refreshSelectionStatus();

    const auto has_selection = !view->selectionModel()->selectedRows().isEmpty();
    track_context_menu_->clear();
    play_selected_action_->setEnabled(view->currentIndex().isValid());
    remove_selected_action_->setEnabled(has_selection);
    track_context_menu_->addAction(play_selected_action_);
    addUpNextActions(track_context_menu_, view);

    track_context_menu_->addSeparator();
    auto* tools = track_context_menu_->addMenu(tr("Tools"));
    tools->addAction(properties_action_);
    tools->addAction(replaygain_action_);
    tools->addAction(convert_action_);

    auto* source_tab = static_cast<ListTab*>(view->property("bench-tab-pointer").value<void*>());
    if (source_tab != nullptr && target.row() < source_tab->model->rowCount()) {
        const auto path =
            source_tab->model->rows()[static_cast<std::size_t>(target.row())].raw_path;
        track_context_menu_->addSeparator();
        for (const bool album : {false, true}) {
            auto* locate = track_context_menu_->addAction(album ? QStringLiteral("Locate album")
                                                                : QStringLiteral("Locate artist"));
            locate->setObjectName(album ? QStringLiteral("action-local-locate-album")
                                        : QStringLiteral("action-local-locate-artist"));
            locate->setEnabled(local_library_ != nullptr);
            connect(locate, &QAction::triggered, this, [this, path, album] {
                if (local_library_ == nullptr)
                    return;
                local_source_tabs_->setCurrentIndex(1);
                refreshActiveContext();
                local_library_->locatePath(path, album);
            });
        }
    }
    addLocalRateMenus(view, source_tab);
    track_context_menu_->addSeparator();
    if (source_tab != nullptr) {
        auto* copy_menu = track_context_menu_->addMenu(QStringLiteral("Copy to list"));
        copy_menu->setObjectName(QStringLiteral("bench-track-copy-menu"));
        auto* move_menu = track_context_menu_->addMenu(QStringLiteral("Move to list"));
        move_menu->setObjectName(QStringLiteral("bench-track-move-menu"));
        for (auto* menu : {copy_menu, move_menu}) {
            const auto move = menu == move_menu;
            auto* create = menu->addAction(tr("New tab…"));
            create->setObjectName(move ? QStringLiteral("action-move-to-new-tab")
                                       : QStringLiteral("action-copy-to-new-tab"));
            connect(create, &QAction::triggered, this, [this, view, move] {
                std::vector<QPersistentModelIndex> selected_rows;
                auto selected = view->selectionModel()->selectedRows(0);
                std::ranges::sort(selected, {}, &QModelIndex::row);
                for (const auto& index : selected)
                    selected_rows.emplace_back(index);
                bool accepted = false;
                const auto name =
                    QInputDialog::getText(this, tr("New tab"), tr("Name:"), QLineEdit::Normal,
                                          tr("Selection"), &accepted)
                        .trimmed();
                if (!accepted || name.isEmpty())
                    return;
                QVariantList rows;
                for (const auto& index : selected_rows) {
                    if (!index.isValid())
                        return;
                    rows.push_back(index.row());
                }
                transferRowsToNewTab(view, rows, move, name);
            });
            if (list_tabs_.size() > 1U)
                menu->addSeparator();
        }
        for (const auto& destination : list_tabs_) {
            if (destination.get() == source_tab) {
                continue;
            }
            const auto target_id = QString::fromStdString(destination->document.id.to_string());
            const auto label = displayText(destination->document.name);
            auto* copy = copy_menu->addAction(label);
            connect(copy, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, false); });
            auto* move = move_menu->addAction(label);
            connect(move, &QAction::triggered, this,
                    [this, view, target_id] { transferSelectedRows(view, target_id, true); });
        }
    }
    track_context_menu_->addAction(remove_selected_action_);
    refreshListHistoryActions();
    track_context_menu_->addSeparator();
    track_context_menu_->addMenu(sort_list_menu_);
    track_context_menu_->addAction(reverse_list_action_);
    track_context_menu_->addAction(shuffle_albums_action_);
    track_context_menu_->addAction(deduplicate_list_action_);
    track_context_menu_->addSeparator();
    track_context_menu_->addAction(undo_list_action_);
    track_context_menu_->addAction(redo_list_action_);
    track_context_menu_->addSeparator();
    addLastFmActions(track_context_menu_, view);
    track_context_menu_->popup(view->viewport()->mapToGlobal(position));
}

void BenchMainWindow::refreshLocalRatings() {
    if (local_library_ == nullptr) {
        return;
    }
    QStringList hashes;
    QSet<QString> unique;
    for (const auto& tab : list_tabs_) {
        for (const auto& hash : tab->model->ratingHashes()) {
            if (!unique.contains(hash)) {
                unique.insert(hash);
                hashes.push_back(hash);
            }
        }
    }
    if (hashes.isEmpty()) {
        return;
    }
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(hashes.size()));
    for (const auto& hash : hashes) {
        keys.push_back(hash.toStdString());
    }
    local_library_->requestRatings(std::move(keys), [this, hashes](std::vector<unsigned> values) {
        if (values.size() != static_cast<std::size_t>(hashes.size())) {
            return;
        }
        QHash<QString, unsigned> ratings;
        ratings.reserve(hashes.size());
        for (qsizetype index = 0; index < hashes.size(); ++index) {
            ratings.insert(hashes.at(index), values[static_cast<std::size_t>(index)]);
        }
        for (const auto& tab : list_tabs_) {
            tab->model->applyRatings(ratings);
        }
    });
}

void BenchMainWindow::addLocalRateMenus(QTableView* view, ListTab* source_tab) {
    if (source_tab)
        addLocalRateMenus(track_context_menu_, view);
}

void BenchMainWindow::addLocalRateMenus(QMenu* menu, QTableView* view) {
    auto* model = view ? qobject_cast<LocalListModel*>(view->model()) : nullptr;
    if (!menu || !model || !view->selectionModel()) {
        return;
    }
    const auto& rows = model->rows();
    QStringList track_hashes;
    QStringList album_hashes;
    QSet<QString> unique_tracks;
    QSet<QString> unique_albums;
    std::optional<unsigned> common_rating;
    bool ratings_match = true;
    std::optional<unsigned> common_album_rating;
    bool album_ratings_match = true;
    for (const auto& index : view->selectionModel()->selectedRows()) {
        if (index.row() < 0 || index.row() >= static_cast<int>(rows.size())) {
            continue;
        }
        const auto& row = rows[static_cast<std::size_t>(index.row())];
        if (!common_rating) {
            common_rating = row.rating;
        } else if (*common_rating != row.rating) {
            ratings_match = false;
        }
        if (!common_album_rating) {
            common_album_rating = row.album_rating;
        } else if (*common_album_rating != row.album_rating) {
            album_ratings_match = false;
        }
        const auto track_hash = QString::fromStdString(row.rating_hash);
        if (!track_hash.isEmpty() && !unique_tracks.contains(track_hash)) {
            unique_tracks.insert(track_hash);
            track_hashes.push_back(track_hash);
        }
        const auto album_hash = QString::fromStdString(row.album_rating_hash);
        if (!album_hash.isEmpty() && !unique_albums.contains(album_hash)) {
            unique_albums.insert(album_hash);
            album_hashes.push_back(album_hash);
        }
    }
    if (track_hashes.isEmpty()) {
        return;
    }
    const auto store_ready = local_library_ != nullptr;
    menu->addSeparator();
    auto* rate_menu = menu->addMenu(QStringLiteral("Rate"));
    rate_menu->setObjectName(QStringLiteral("bench-local-rate-menu"));
    rate_menu->setEnabled(store_ready);
    auto* album_rate_menu = menu->addMenu(QStringLiteral("Rate album"));
    album_rate_menu->setObjectName(QStringLiteral("bench-local-album-rate-menu"));
    album_rate_menu->setEnabled(store_ready && !album_hashes.isEmpty());
    const auto make_rating_action = [](QMenu* target_menu, const unsigned rating) -> QAction* {
        if (rating == 0U) {
            auto* unrate = target_menu->addAction(ui::ratingMenuLabel(rating));
            unrate->setCheckable(true);
            return unrate;
        }
        auto* stars = new ui::RatingMenuAction(rating, target_menu);
        target_menu->addAction(stars);
        return stars;
    };
    for (unsigned rating = 0U; rating <= 10U; rating += 2U) {
        auto* rate = make_rating_action(rate_menu, rating);
        rate->setObjectName(QStringLiteral("action-local-rate-%1").arg(rating));
        rate->setChecked(ratings_match && common_rating == rating);
        connect(rate, &QAction::triggered, this,
                [this, model = QPointer{model}, track_hashes, rating] {
                    if (local_library_ == nullptr) {
                        return;
                    }
                    QHash<QString, unsigned> applied;
                    for (const auto& hash : track_hashes) {
                        local_library_->storeRating(hash.toStdString(), false, rating);
                        applied.insert(hash, rating);
                    }
                    for (const auto& tab : list_tabs_) {
                        tab->model->applyRatings(applied);
                    }
                    if (model)
                        model->applyRatings(applied);
                });
        auto* album_rate = make_rating_action(album_rate_menu, rating);
        album_rate->setObjectName(QStringLiteral("action-local-album-rate-%1").arg(rating));
        album_rate->setChecked(album_ratings_match && common_album_rating == rating);
        connect(album_rate, &QAction::triggered, this, [this, album_hashes, rating] {
            if (local_library_ == nullptr) {
                return;
            }
            for (const auto& hash : album_hashes) {
                local_library_->storeRating(hash.toStdString(), true, rating);
            }
        });
    }
}

void BenchMainWindow::showFolderContextMenu(const QPoint& position) {
    if (folder_context_menu_ == nullptr) {
        return;
    }
    const auto target = folder_view_->indexAt(position);
    if (!target.isValid()) {
        return;
    }
    folder_view_->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
    const auto directory = folder_model_->isDirectory(target);
    folder_add_to_list_action_->setText(directory ? QStringLiteral("Add folder to current list")
                                                  : QStringLiteral("Add file to current list"));
    folder_toggle_expanded_action_->setText(
        folder_view_->isExpanded(target) ? QStringLiteral("Collapse") : QStringLiteral("Expand"));
    folder_toggle_expanded_action_->setEnabled(directory);
    folder_context_menu_->clear();
    folder_context_menu_->addAction(folder_add_to_list_action_);
    if (directory) {
        folder_context_menu_->addAction(folder_toggle_expanded_action_);
        folder_context_menu_->addAction(folder_bookmark_add_action_);
    }
    folder_context_menu_->popup(folder_view_->viewport()->mapToGlobal(position));
}

void BenchMainWindow::playCurrentRow() {
    auto* tab = currentListTab();
    if (tab != nullptr && tab->view->currentIndex().isValid()) {
        playRow(*tab, tab->view->currentIndex().row());
    }
}

void BenchMainWindow::refreshListHistoryActions() {
    if (undo_list_action_ == nullptr || redo_list_action_ == nullptr)
        return;
    const auto* tab = currentListTab();
    const auto* model = tab == nullptr ? nullptr : tab->model;
    if (shuffle_albums_action_) {
        shuffle_albums_action_->setEnabled(model != nullptr && model->rowCount() > 1);
    }
    if (sort_list_menu_) {
        const auto editable = model != nullptr && model->rowCount() > 1;
        sort_list_menu_->setEnabled(editable);
        reverse_list_action_->setEnabled(editable);
        deduplicate_list_action_->setEnabled(editable);
    }
    const auto cross_tab_undo = canReplayCrossTabMove(true);
    const auto cross_tab_redo = canReplayCrossTabMove(false);
    undo_list_action_->setEnabled(cross_tab_undo || (model != nullptr && model->canUndo()));
    redo_list_action_->setEnabled(cross_tab_redo || (model != nullptr && model->canRedo()));
    undo_list_action_->setText(cross_tab_undo ? tr("Undo Move tracks between tabs")
                               : model != nullptr && model->canUndo()
                                   ? tr("Undo %1").arg(model->undoLabel())
                                   : tr("Undo list edit"));
    redo_list_action_->setText(cross_tab_redo ? tr("Redo Move tracks between tabs")
                               : model != nullptr && model->canRedo()
                                   ? tr("Redo %1").arg(model->redoLabel())
                                   : tr("Redo list edit"));
}

void BenchMainWindow::replayListEdit(const bool undo) {
    auto* tab = currentListTab();
    if (tab == nullptr)
        return;
    if (replayCrossTabMove(undo) || (undo ? tab->model->undo() : tab->model->redo())) {
        markTabDirty(*tab);
        enqueueUnprobedRows(*tab);
        syncArtwork(*tab);
        refreshSelectionStatus();
    }
    refreshListHistoryActions();
}

void BenchMainWindow::removeSelectedRows() {
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        return;
    }
    std::vector<int> rows;
    const auto selection = tab->view->selectionModel()->selectedRows();
    rows.reserve(static_cast<std::size_t>(selection.size()));
    for (const auto& index : selection) {
        rows.push_back(index.row());
    }
    if (rows.empty()) {
        return;
    }
    tab->model->removeRowIndexes(std::move(rows));
    markTabDirty(*tab);
}

void BenchMainWindow::transferSelectedRows(QTableView* source, const QString& target_id,
                                           const bool move) {
    if (source == nullptr || source->selectionModel() == nullptr) {
        return;
    }
    auto selected = source->selectionModel()->selectedRows(0);
    std::ranges::sort(selected, {}, &QModelIndex::row);
    QVariantList rows;
    rows.reserve(selected.size());
    for (const auto& index : selected) {
        rows.push_back(index.row());
    }
    if (!rows.isEmpty()) {
        static_cast<void>(transferRows(source, rows, target_id, move, -1));
    }
}

} // namespace trackknife::bench
