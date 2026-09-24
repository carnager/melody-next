// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QAction>
#include <QFutureWatcher>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QtConcurrentRun>

namespace trackknife::bench {
void BenchMainWindow::showDynamicPlaylists() {
    // As Search does: opened from a remote tab, it starts on the remote's
    // library; the dropdown switches.
    const auto* current = currentListTab();
    const bool from_remote =
        current != nullptr && current->document.remote && remote_catalogue_source_ != nullptr;
    if (auto* existing = findChild<DynamicPlaylistDialog*>()) {
        if (existing->authorityValid()) {
            existing->followLibrary(from_remote);
            existing->show();
            existing->raise();
            existing->activateWindow();
            return;
        }
        existing->close();
    }
    // One set of definitions: a rule reads the same on either library.
    const auto profile = QStringLiteral("local");
    // ADR-0220: through the catalogue source, like every other library read.
    // Opening the database directly here meant that with an engine
    // configured, dynamic playlists quietly queried this process's library
    // instead of the engine's.
    DynamicPlaylistDialog::LibrarySearch search =
        [this](const bool remote, query::CompiledTkq compiled,
               core::CancellationToken cancellation,
               DynamicPlaylistService::Completion completion) {
            auto* catalogues = remote ? remote_catalogue_source_.get() : catalogue_source_.get();
            if (catalogues == nullptr) {
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "No remote engine is configured",
                                                       .context = {}}));
                return;
            }
            auto* watcher = new QFutureWatcher<DynamicPlaylistService::Result>(this);
            connect(watcher, &QFutureWatcher<DynamicPlaylistService::Result>::finished, this,
                    [watcher, completion = std::move(completion)] {
                        completion(watcher->future().takeResult());
                        watcher->deleteLater();
                    });
            watcher->setFuture(
                QtConcurrent::run([catalogues, compiled = std::move(compiled),
                                   cancellation]() -> DynamicPlaylistService::Result {
                    const auto catalogue = catalogues->open();
                    return queryDynamicLibrary(*catalogue, compiled, cancellation);
                }));
        };
    auto* dialog = new DynamicPlaylistDialog(
        profile, remote_catalogue_source_ ? remote_catalogue_source_->name() : QString{},
        std::move(search), this);
    auto layout = defaultTrackViewLayout(ui::TrackViewPresentation::plain_columns);
    applyTrackViewLayout(dialog->view(), layout, layout);
    auto* result_view = dialog->view();
    if (auto* local_model = qobject_cast<LocalListModel*>(result_view->model()))
        local_model->setListeningHistoryService(persistence_);
    for (const bool prepend : {true, false}) {
        auto* original = findChild<QAction*>(prepend ? QStringLiteral("action-queue-next")
                                                     : QStringLiteral("action-queue-end"));
        if (!original)
            continue;
        auto* scoped = new QAction(original->text(), result_view);
        scoped->setShortcuts(original->shortcuts());
        scoped->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        result_view->addAction(scoped);
        connect(scoped, &QAction::triggered, dialog, [this, dialog, prepend] {
            if (dialog->authorityValid())
                enqueueUpNext(dialog->view(), prepend);
        });
    }
    connect(dialog, &DynamicPlaylistDialog::playRequested, this, [this, dialog, layout](int row) {
        if (!dialog->authorityValid() || row < 0)
            return;
        const auto name = dialog->playlistName().isEmpty()
                              ? tr("Dynamic playback")
                              : dialog->playlistName() + tr(" — playback");
        const auto& rows = dialog->tracks();
        if (static_cast<std::size_t>(row) >= rows.size())
            return;
        auto* destination =
            addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                 .kind = persistence::ListKind::scratch,
                                                 .name = utf8Bytes(name),
                                                 .pinned = false,
                                                 .dirty = false,
                                                 .items = {},
                                                 .remote = dialog->remote()},
                       false);
        destination->model->replaceRows(rows);
        dialog->setProperty("playback-context",
                            QString::fromStdString(destination->document.id.to_string()));
        applyTrackViewLayout(*destination, layout);
        markTabDirty(*destination);
        syncArtwork(*destination);
        schedulePersist();
        playRow(*destination, row);
    });
    const auto markers = [this, dialog] {
        auto* model = qobject_cast<LocalListModel*>(dialog->view()->model());
        int occurrence = 0;
        if (dialog->property("playback-context").toString() ==
            document_text(playback_.anchors.document)) {
            if (const auto* tab = tabForDocument(playback_.anchors.document);
                tab && tab->model->rowCount() <= 500)
                for (int i = 0; i < playback_.row && i < tab->model->rowCount(); ++i)
                    if (tab->model->source(i) == playback_.anchors.source)
                        ++occurrence;
        }
        int hint = -1;
        for (int i = 0; i < model->rowCount(); ++i)
            if (model->source(i) == playback_.anchors.source && occurrence-- == 0) {
                hint = i;
                break;
            }
        model->setCurrentSource(hint >= 0 ? playback_.anchors.source : LocalTrackSource{}, hint);
    };
    connect(dialog, &DynamicPlaylistDialog::resultsChanged, dialog, markers);
    auto* marker_timer = new QTimer(dialog);
    marker_timer->setInterval(500);
    connect(marker_timer, &QTimer::timeout, dialog, markers);
    marker_timer->start();
    dialog->view()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dialog->view(), &QWidget::customContextMenuRequested, dialog,
            [this, dialog](const QPoint& position) {
                auto* view = dialog->view();
                const auto index = view->indexAt(position);
                if (!index.isValid())
                    return;
                if (!view->selectionModel()->isRowSelected(index.row(), {}))
                    view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
                view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                QMenu menu(view);
                menu.setObjectName(QStringLiteral("dynamic-track-menu"));
                auto* play = menu.addAction(tr("Play"));
                play->setObjectName(QStringLiteral("dynamic-play"));
                connect(play, &QAction::triggered, dialog, &DynamicPlaylistDialog::playCurrent);
                addUpNextActions(&menu, view);
                for (const bool album : {false, true}) {
                    auto* locate = menu.addAction(album ? tr("Go to album") : tr("Go to artist"));
                    auto* model = qobject_cast<LocalListModel*>(view->model());
                    const auto path = model->rawPath(index.row());
                    // In the library the result came from.
                    auto* library = dialog->remote() ? remote_library_ : local_library_;
                    locate->setEnabled(library != nullptr);
                    connect(locate, &QAction::triggered, dialog,
                            [library = QPointer{library}, path, album] {
                                if (library)
                                    library->locatePath(path, album);
                            });
                }
                auto* tools = menu.addMenu(tr("Tools"));
                connect(tools->addAction(tr("Edit tags…")), &QAction::triggered, dialog,
                        [this, view] { showMetadataForView(view); });
                connect(tools->addAction(tr("ReplayGain…")), &QAction::triggered, dialog,
                        [this, view] { showReplayGainForView(view); });
                connect(tools->addAction(tr("Convert files…")), &QAction::triggered, dialog,
                        [this, view] { showConvertForView(view); });
                addLocalRateMenus(&menu, view);
                auto* copy = menu.addMenu(tr("Copy to list"));
                connect(copy->addAction(tr("New tab…")), &QAction::triggered, dialog, [this, view] {
                    auto selected = view->selectionModel()->selectedRows();
                    std::ranges::sort(selected, {}, &QModelIndex::row);
                    QVariantList rows;
                    for (const auto& selected_row : selected)
                        rows.push_back(selected_row.row());
                    transferRowsToNewTab(view, rows, false, tr("Selection"));
                });
                for (const auto& tab : list_tabs_) {
                    auto* choice = copy->addAction(displayText(tab->document.name));
                    connect(
                        choice, &QAction::triggered, dialog,
                        [this, view, id = QString::fromStdString(tab->document.id.to_string())] {
                            transferSelectedRows(view, id, false);
                        });
                }
                menu.addSeparator();
                auto* edit = menu.addAction(tr("Open editable snapshot…"));
                edit->setToolTip(tr("Manual edits apply to a separate list; the dynamic "
                                    "definition stays unchanged."));
                connect(edit, &QAction::triggered, dialog, [dialog] {
                    emit dialog->snapshotRequested(dialog->playlistName(), dialog->tracks());
                });
                menu.addSeparator();
                addLastFmActions(&menu, view);
                menu.exec(view->viewport()->mapToGlobal(position));
            });
    // Rules follow changes to the library they read, and only that one.
    connect(persistence_, &ui::ListPersistenceService::listeningHistoryChanged, dialog,
            &DynamicPlaylistDialog::libraryChanged);
    for (auto* library : {local_library_, remote_library_}) {
        if (library == nullptr)
            continue;
        const bool remote = library == remote_library_;
        const auto changed = [dialog, remote] {
            if (dialog->remote() == remote)
                dialog->libraryChanged();
        };
        connect(library, &LocalLibraryPanel::ratingsChanged, dialog, changed);
        connect(library, &LocalLibraryPanel::libraryContentChanged, dialog, changed);
    }
    connect(dialog, &DynamicPlaylistDialog::snapshotRequested, this,
            [this, dialog, layout](const QString& name,
                                   const DynamicPlaylistService::Tracks& tracks) {
                const auto title =
                    name.isEmpty() ? QStringLiteral("Dynamic playlist snapshot") : name;
                auto* destination =
                    addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = utf8Bytes(title),
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {},
                                                         .remote = dialog->remote()},
                               true);
                applyTrackViewLayout(*destination, layout);
                destination->model->replaceRows(tracks);
                markTabDirty(*destination);
                syncArtwork(*destination);
                schedulePersist();
            });
    dialog->followLibrary(from_remote);
    dialog->show();
}
} // namespace trackknife::bench
