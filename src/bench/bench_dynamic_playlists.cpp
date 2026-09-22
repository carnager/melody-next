// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "quick/mpd_queue_model.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq_melody.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/rating_stars.hpp"
#include <QAction>
#include <QFutureWatcher>
#include <QMenu>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QtConcurrentRun>

namespace trackknife::bench {
QStringList BenchMainWindow::mpdDynamicSnapshots() const {
    return QSettings{}
        .value(QStringLiteral("mpd/dynamic-snapshots/%1").arg(mpd_controller_->profileId()))
        .toStringList();
}

void BenchMainWindow::setMpdDynamicSnapshot(const QString& name, const bool enabled) {
    auto names = mpdDynamicSnapshots();
    names.removeAll(name);
    if (enabled)
        names.push_back(name);
    QSettings{}.setValue(
        QStringLiteral("mpd/dynamic-snapshots/%1").arg(mpd_controller_->profileId()), names);
}

void BenchMainWindow::showDynamicPlaylists() {
    const bool mpd = isMpdContext();
    if (mpd && (!mpd_controller_->connected() ||
                !mpd_controller_->supportsCommand(QStringLiteral("search")))) {
        statusBar()->showMessage(
            QStringLiteral(
                "Connect to an MPD/Melody server with library search to use dynamic playlists."),
            8000);
        return;
    }
    if (auto* existing = findChild<DynamicPlaylistDialog*>()) {
        if (existing->authorityValid()) {
            existing->show();
            existing->raise();
            existing->activateWindow();
            return;
        }
        existing->close();
    }
    const auto profile =
        mpd ? QStringLiteral("mpd/") + mpd_controller_->profileId() : QStringLiteral("local");
    DynamicPlaylistService::Search search;
    if (mpd) {
        const auto expected = mpd_controller_->profileId();
        search = [this, expected](query::CompiledTkq compiled, core::CancellationToken cancellation,
                                  DynamicPlaylistService::Completion completion) {
            if (cancellation.is_cancellation_requested())
                return;
            if (!mpd_controller_->connected() || expected != mpd_controller_->profileId()) {
                completion(std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                                       .message = "Server profile changed",
                                                       .context = {}}));
                return;
            }
            if (!mpd_controller_->supportsServerQueries()) {
                for (const auto& predicate : compiled.predicates) {
                    const auto& field = predicate.field;
                    if (field == "rating" || field == "albumrating" || field == "codec" ||
                        field == "samplerate" || field == "bitspersample" || field == "channels" ||
                        field == "lengthms") {
                        completion(std::unexpected(core::Error{
                            .code = core::ErrorCode::unsupported,
                            .message = "This rule needs Melody's rating/technical filters; stock "
                                       "MPD supports tag rules and Last.fm matching",
                            .context = {}}));
                        return;
                    }
                }
            }
            const auto translated = query::translate_tkq_to_melody(
                compiled, mpd_controller_->supportsCommand(QStringLiteral("filtergrammar")),
                mpd_controller_->supportsCommand(QStringLiteral("melody_history_filters")),
                mpd_controller_->supportsCommand(QStringLiteral("melody_history_sort")));
            if (!translated) {
                completion(std::unexpected(translated.error()));
                return;
            }
            mpd_controller_->searchServerExpression(
                QString::fromStdString(translated->filter_expression),
                QString::fromStdString(translated->sort),
                [completion = std::move(completion)](
                    core::Result<std::vector<trackknife::mpd::Track>> result) {
                    if (!result)
                        completion(std::unexpected(result.error()));
                    else if (result->size() >= 20'000U)
                        completion(std::unexpected(
                            core::Error{.code = core::ErrorCode::limit_exceeded,
                                        .message = "The server query reached 20,000 tracks. Narrow "
                                                   "the rule before generating this playlist.",
                                        .context = {}}));
                    else
                        completion(DynamicPlaylistService::Tracks{std::move(*result)});
                });
        };
    } else {
        search = [this](query::CompiledTkq compiled, core::CancellationToken cancellation,
                        DynamicPlaylistService::Completion completion) {
            auto* watcher = new QFutureWatcher<DynamicPlaylistService::Result>(this);
            connect(watcher, &QFutureWatcher<DynamicPlaylistService::Result>::finished, this,
                    [watcher, completion = std::move(completion)] {
                        completion(watcher->future().takeResult());
                        watcher->deleteLater();
                    });
            watcher->setFuture(
                QtConcurrent::run([database = database_path_, compiled = std::move(compiled),
                                   cancellation]() -> DynamicPlaylistService::Result {
                    return queryDynamicLocalLibrary(database, compiled, cancellation);
                }));
        };
    }
    auto* dialog = new DynamicPlaylistDialog(
        profile, mpd ? QStringLiteral("MPD/Melody library") : QStringLiteral("Local library"),
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
    connect(
        dialog, &DynamicPlaylistDialog::playRequested, this, [this, dialog, mpd, layout](int row) {
            if (!dialog->authorityValid() || row < 0)
                return;
            const auto name = dialog->playlistName().isEmpty()
                                  ? tr("Dynamic playback")
                                  : dialog->playlistName() + tr(" — playback");
            if (mpd) {
                if (!mpd_controller_->connected())
                    return;
                const auto& tracks =
                    std::get<std::vector<trackknife::mpd::Track>>(dialog->tracks());
                if (static_cast<std::size_t>(row) >= tracks.size())
                    return;
                QStringList uris;
                for (const auto& track : tracks)
                    uris.push_back(displayText(track.uri));
                if (mpd_controller_->supportsPlaybackContexts()) {
                    // Use a fresh server-owned list; never overwrite another playing snapshot.
                    auto title = name;
                    int suffix = 2;
                    while (mpdPlaylistNames().contains(title) || mpdPlaylistTabNamed(title))
                        title = name + QStringLiteral(" (%1)").arg(suffix++);
                    setMpdDynamicSnapshot(title, true);
                    createScratchListTab(title, uris, false);
                    dialog->setProperty("playback-context", title);
                    mpd_controller_->playListContext(title, row);
                } else {
                    mpd_controller_->replaceQueueWithUrisAndPlayAt(uris, row);
                }
            } else {
                const auto& rows = std::get<std::vector<LocalTrackRow>>(dialog->tracks());
                if (static_cast<std::size_t>(row) >= rows.size())
                    return;
                auto* destination =
                    addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = utf8Bytes(name),
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {}},
                               false);
                destination->model->replaceRows(rows);
                dialog->setProperty("playback-context",
                                    QString::fromStdString(destination->document.id.to_string()));
                applyTrackViewLayout(*destination, layout);
                markTabDirty(*destination);
                syncArtwork(*destination);
                schedulePersist();
                playRow(*destination, row);
            }
        });
    const auto markers = [this, dialog, mpd] {
        if (mpd) {
            auto* model = qobject_cast<quick::MpdQueueModel*>(dialog->view()->model());
            std::optional<int> current;
            int occurrence = 0;
            const auto context = dialog->property("playback-context").toString();
            if (!context.isEmpty() && mpd_controller_->activeContextName() == context) {
                if (const auto* tab = mpdPlaylistTabNamed(context);
                    tab && tab->model->rowCount() <= 500) {
                    for (int i = 0;
                         i < mpd_controller_->songPosition() && i < tab->model->rowCount(); ++i)
                        if (displayText(tab->model->trackAt(i)->uri) ==
                            mpd_controller_->nowPlayingUri())
                            ++occurrence;
                }
            }
            if (mpd_controller_->playing() || mpd_controller_->paused()) {
                for (int i = 0; i < model->rowCount(); ++i)
                    if (displayText(model->trackAt(i)->uri) == mpd_controller_->nowPlayingUri() &&
                        occurrence-- == 0) {
                        current = i;
                        break;
                    }
            }
            model->setCurrentRow(current);
        } else {
            auto* model = qobject_cast<LocalListModel*>(dialog->view()->model());
            int occurrence = 0;
            if (dialog->property("playback-context").toString() == document_text(playback_.anchors.document)) {
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
        }
    };
    connect(dialog, &DynamicPlaylistDialog::resultsChanged, dialog, markers);
    auto* marker_timer = new QTimer(dialog);
    marker_timer->setInterval(500);
    connect(marker_timer, &QTimer::timeout, dialog, markers);
    marker_timer->start();
    dialog->view()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        dialog->view(), &QWidget::customContextMenuRequested, dialog,
        [this, dialog, mpd](const QPoint& position) {
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
                if (mpd) {
                    const auto artist =
                        index.siblingAtColumn(0).data(ui::track_album_artist_role).toString();
                    const auto album_name =
                        album ? index.siblingAtColumn(ui::track_album_column).data().toString()
                              : QString{};
                    locate->setEnabled(!artist.isEmpty());
                    connect(locate, &QAction::triggered, dialog, [this, artist, album_name] {
                        goToMpdLibraryEntry(artist, album_name);
                    });
                } else {
                    auto* model = qobject_cast<LocalListModel*>(view->model());
                    const auto path = model->rawPath(index.row());
                    locate->setEnabled(local_library_ != nullptr);
                    connect(locate, &QAction::triggered, dialog, [this, path, album] {
                        if (local_library_)
                            local_library_->locatePath(path, album);
                    });
                }
            }
            if (mpd) {
                const auto tracks = selectedMpdViewTracks(view);
                QStringList uris;
                for (const auto& track : tracks)
                    uris.push_back(displayText(track.uri));
                auto* tools = menu.addMenu(tr("Tools"));
                addMappedLocalTrackActions(tools, uris, QStringLiteral("dynamic-mapped-"));
                if (mpd_controller_->supportsRatings()) {
                    auto* rate = menu.addMenu(tr("Rate"));
                    for (unsigned rating = 0; rating <= 10; rating += 2) {
                        auto* choice = rate->addAction(ui::ratingMenuLabel(rating));
                        connect(choice, &QAction::triggered, dialog, [this, tracks, rating] {
                            mpd_controller_->setTracksRating(tracks, static_cast<int>(rating));
                        });
                    }
                }
                if (mpd_controller_->supportsAlbumRatings() && !tracks.empty()) {
                    auto* rate = menu.addMenu(tr("Rate album"));
                    for (unsigned rating = 0; rating <= 10; rating += 2) {
                        auto* choice = rate->addAction(ui::ratingMenuLabel(rating));
                        connect(choice, &QAction::triggered, dialog, [this, tracks, rating] {
                            QSet<QString> seen;
                            for (const auto& track : tracks) {
                                const auto key = quick::MpdQueueModel::albumGroupKey(track);
                                if (seen.contains(key))
                                    continue;
                                seen.insert(key);
                                const auto value = [&track](const char* field) {
                                    return displayText(
                                        std::string{track.metadata.first(field).value_or("")});
                                };
                                const auto artist = value("AlbumArtist").isEmpty()
                                                        ? value("Artist")
                                                        : value("AlbumArtist");
                                if (!artist.isEmpty() && !value("Album").isEmpty())
                                    mpd_controller_->setMelodyAlbumRating(artist, value("Album"),
                                                                          value("Date"),
                                                                          static_cast<int>(rating));
                            }
                        });
                    }
                }
                addSendToTabMenu(&menu, [this, view] { return selectedMpdViewTracks(view); });
            } else {
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
            }
            menu.addSeparator();
            auto* edit = menu.addAction(tr("Open editable snapshot…"));
            edit->setToolTip(tr(
                "Manual edits apply to a separate list; the dynamic definition stays unchanged."));
            connect(edit, &QAction::triggered, dialog, [dialog] {
                emit dialog->snapshotRequested(dialog->playlistName(), dialog->tracks());
            });
            menu.addSeparator();
            addLastFmActions(&menu, view);
            menu.exec(view->viewport()->mapToGlobal(position));
        });
    connect(tabs_, &QTabWidget::currentChanged, dialog, [this, dialog, mpd](int) {
        if (isMpdContext() != mpd)
            dialog->close();
    });
    if (mpd) {
        const auto expected = mpd_controller_->profileId();
        connect(mpd_controller_, &quick::MpdProbeController::stateChanged, dialog,
                [this, dialog, expected] {
                    if (!mpd_controller_->connected() || mpd_controller_->profileId() != expected)
                        dialog->invalidateAuthority();
                });
        connect(mpd_controller_, &quick::MpdProbeController::serverDatabaseChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
        connect(mpd_controller_, &quick::MpdProbeController::listeningStatisticsChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
    } else if (local_library_) {
        connect(persistence_, &ui::ListPersistenceService::listeningHistoryChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
        connect(local_library_, &LocalLibraryPanel::ratingsChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
        connect(local_library_, &LocalLibraryPanel::libraryContentChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
    }
    connect(
        dialog, &DynamicPlaylistDialog::snapshotRequested, this,
        [this, mpd, layout](const QString& name, const DynamicPlaylistService::Tracks& tracks) {
            const auto title = name.isEmpty() ? QStringLiteral("Dynamic playlist snapshot") : name;
            if (mpd) {
                if (isMpdContext()) {
                    auto unique_title = title;
                    int suffix = 2;
                    while (mpdPlaylistNames().contains(unique_title) ||
                           mpdPlaylistTabNamed(unique_title))
                        unique_title = title + QStringLiteral(" (%1)").arg(suffix++);
                    setMpdDynamicSnapshot(unique_title, true);
                    openMpdSearchTab(unique_title,
                                     std::get<std::vector<trackknife::mpd::Track>>(tracks), true);
                    if (auto* tab = mpdPlaylistTabNamed(unique_title))
                        applyTrackViewLayout(tab->view, tab->view_layout, layout);
                }
            } else {
                auto* destination =
                    addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                         .kind = persistence::ListKind::scratch,
                                                         .name = utf8Bytes(title),
                                                         .pinned = false,
                                                         .dirty = false,
                                                         .items = {}},
                               true);
                applyTrackViewLayout(*destination, layout);
                destination->model->replaceRows(std::get<std::vector<LocalTrackRow>>(tracks));
                markTabDirty(*destination);
                syncArtwork(*destination);
                schedulePersist();
            }
        });
    connect(dialog, &DynamicPlaylistDialog::appendRequested, this,
            [this, mpd](const DynamicPlaylistService::Tracks& tracks) {
                if (!mpd || !isMpdContext())
                    return;
                QStringList uris;
                for (const auto& track : std::get<std::vector<trackknife::mpd::Track>>(tracks))
                    uris.push_back(displayText(track.uri));
                mpd_controller_->addUris(uris, false);
            });
    dialog->show();
}
} // namespace trackknife::bench
