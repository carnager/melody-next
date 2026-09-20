// SPDX-License-Identifier: GPL-3.0-only
#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/local_library_panel.hpp"
#include "quick/mpd_probe_controller.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq_melody.hpp"
#include "uicommon/queue_table_view.hpp"
#include <QFutureWatcher>
#include <QMenu>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
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
                compiled, mpd_controller_->supportsCommand(QStringLiteral("filtergrammar")));
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
                QMenu menu(view);
                addUpNextActions(&menu, view);
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
    } else if (local_library_) {
        connect(local_library_, &LocalLibraryPanel::ratingsChanged, dialog,
                &DynamicPlaylistDialog::libraryChanged);
    }
    connect(dialog, &DynamicPlaylistDialog::snapshotRequested, this,
            [this, mpd, layout](const QString& name, const DynamicPlaylistService::Tracks& tracks) {
                const auto title =
                    name.isEmpty() ? QStringLiteral("Dynamic playlist snapshot") : name;
                if (mpd) {
                    if (isMpdContext()) {
                        setMpdDynamicSnapshot(title, true);
                        openMpdSearchTab(
                            title, std::get<std::vector<trackknife::mpd::Track>>(tracks), true);
                        if (auto* tab = mpdPlaylistTabNamed(title))
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
