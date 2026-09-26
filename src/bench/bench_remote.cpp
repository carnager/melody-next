// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227: the remote engine beside this computer's. Its tabs list files on
// its machine and play there; its library sits beside this computer's in the
// source switch. Nothing here reads the remote files: what they are comes
// from the engine that has them.

#include "bench/bench_main_window.hpp"
#include "uicommon/queue_table_view.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/local_library_panel.hpp"

#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>

namespace trackknife::bench {

BenchMainWindow::ListTab* BenchMainWindow::remoteQueueTab() {
    for (const auto& tab : list_tabs_) {
        if (tab->document.remote) {
            return tab.get();
        }
    }
    if (remote_catalogue_source_ == nullptr) {
        return nullptr;
    }
    // Opened on first connection, named after the remote so it reads as a
    // place rather than a list.
    auto* tab =
        addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                             .kind = persistence::ListKind::scratch,
                                             .name = utf8Bytes(remote_catalogue_source_->name()),
                                             .pinned = false,
                                             .dirty = false,
                                             .items = {},
                                             .remote = true},
                   false);
    schedulePersist();
    return tab;
}

void BenchMainWindow::connectRemoteEngine() {
    remote_catalogue_source_ =
        std::make_unique<CatalogueSource>(database_path_, CatalogueSource::Role::remote);
    if (!remote_catalogue_source_->configured()) {
        remote_catalogue_source_.reset();
        return;
    }
    remote_playback_ = new EnginePlayback(*remote_catalogue_source_, this);
    if (list_sync_ != nullptr) {
        list_sync_->setEngines(local_playback_, remote_playback_);
        connect(remote_playback_, &EnginePlayback::listChanged, this,
                [this](const QString& id, const quint64 revision, const bool deleted) {
                    list_sync_->listChanged(remote_playback_, id, revision, deleted);
                });
        connect(remote_playback_, &EnginePlayback::connected, this, [this] {
            list_sync_->reconnected(remote_playback_);
            flushEngineRelocations();
        });
    }
    connect(remote_playback_, &EnginePlayback::changed, this, [this] {
        followIfStartedElsewhere(remote_playback_);
        if (transport_ == remote_playback_) {
            refreshTransport();
        }
    });
    connect(remote_playback_, &EnginePlayback::ratingChanged, this,
            [this](const QString& hash, const unsigned rating) {
                adoptEngineRating(true, hash, rating);
            });
    connect(remote_playback_, &EnginePlayback::failed, this, [this](const QString& message) {
        statusBar()->showMessage(QStringLiteral("Engine: %1").arg(message), 8'000);
    });
    const auto attached = [this] {
        // What it is doing now is not news; a start after this is.
        rememberEngineState(remote_playback_);
        // Connected, the remote says what it is called: the library tab shows
        // that rather than its address (an engine too old to say keeps it).
        static_cast<void>(remote_catalogue_source_->open());
        for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
             ++index) {
            if (local_source_tabs_->tabData(index).toString() == QStringLiteral("remote")) {
                local_source_tabs_->setTabText(index, remote_catalogue_source_->name());
            }
        }
        // The remote tabs were restored before there was a remote to ask for
        // their covers and missing tags -- or while it was away: they ask now.
        for (auto& tab : list_tabs_) {
            if (tab->document.remote) {
                // Named now that the remote has said its name.
                static_cast<ui::QueueTableView*>(tab->view)
                    ->setEmptyMessage(emptyListTitle(true), emptyListHint(true));
                enqueueUnprobedRows(*tab);
                syncArtwork(*tab);
            }
        }
        refreshLocalRatings();
        // A remote that restarted holds the Up Next it saved; this window's
        // is the one the user sees, so it is stated again.
        engine_requests_.clear();
        syncEngineRequests();
        // Its tab, named after it -- by address if it was made while the
        // remote was away, which is renamed now that it has said its name.
        // A name someone chose is theirs, and kept.
        if (auto* tab = remoteQueueTab(); tab != nullptr) {
            const auto address = remote_catalogue_source_->addressName();
            const auto announced = remote_catalogue_source_->name();
            if (displayText(tab->document.name) == address && announced != address) {
                tab->document.name = utf8Bytes(announced);
                refreshTabChrome(*tab);
                schedulePersist();
            }
        }
        // Music the remote was already playing is followed, unless this
        // computer is playing: then that is what the transport shows, and
        // the remote waits until one of its tabs is played.
        const auto remote = remote_playback_->state();
        const bool local_idle = local_playback_ == nullptr ||
                                local_playback_->state().status == QStringLiteral("stopped");
        if (!remote.entry.isEmpty() && local_idle && transport_ != remote_playback_) {
            followPlayback(remote_playback_);
        }
        if (transport_ == remote_playback_) {
            reattachToEngine();
        }
    };
    connect(remote_playback_, &EnginePlayback::connected, this, attached);
    if (remote_playback_->active()) {
        attached();
    } else {
        // Still offered, so a remote that is down now has its tab to come
        // back to.
        static_cast<void>(remoteQueueTab());
    }

    remote_library_ = new LocalLibraryPanel(*remote_catalogue_source_, source_stack_);
    remote_library_->setObjectName(QStringLiteral("bench-remote-library"));
    source_stack_->addWidget(remote_library_);
    // Ratings set in remote tabs are stored on the remote, and read from it.
    connect(remote_library_, &LocalLibraryPanel::ratingsChanged, this,
            &BenchMainWindow::refreshLocalRatings);
    refreshLocalRatings();
    const auto index = local_source_tabs_->addTab(remote_catalogue_source_->name());
    local_source_tabs_->setTabData(index, QStringLiteral("remote"));
    local_source_tabs_->setTabToolTip(index, remote_catalogue_source_->describe());
    // Now that there is a remote to show instead, or it was the one chosen.
    applyLocalLibraryVisibility();
    selectPreferredSource();

    connect(
        remote_library_, &LocalLibraryPanel::actionRequested, this,
        [this](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
            if (entries.empty()) {
                return;
            }
            if (action == LocalLibraryAction::request_next ||
                action == LocalLibraryAction::request_end) {
                remote_library_->resolveEntryRows(
                    std::move(entries), [this, action](std::vector<LocalTrackRow> rows) {
                        enqueueLocalRequests(std::move(rows),
                                             action == LocalLibraryAction::request_next ? 0 : -1,
                                             true);
                    });
                return;
            }
            // Into the remote tab on screen, or the remote's own: a
            // remote file never lands in a local tab.
            auto* target = currentListTab();
            if (target == nullptr || !target->document.remote) {
                target = remoteQueueTab();
            }
            if (target == nullptr) {
                return;
            }
            if (action == LocalLibraryAction::new_list) {
                target = addListTab(
                    persistence::ListDocument{.id = core::StableId::random(),
                                              .kind = persistence::ListKind::scratch,
                                              .name = entries.size() == 1U ? entries.front().label
                                                                           : "Library selection",
                                              .pinned = false,
                                              .dirty = false,
                                              .items = {},
                                              .remote = true},
                    true);
                schedulePersist();
            }
            int insertion = -1;
            if (action == LocalLibraryAction::next) {
                insertion = playback_.anchors.document == target->document.id ? playback_.row + 1
                            : target->view->currentIndex().isValid()
                                ? target->view->currentIndex().row() + 1
                                : 0;
            }
            const auto id = QString::fromStdString(target->document.id.to_string());
            remote_library_->resolveEntryRows(
                std::move(entries), [this, id, action, insertion](std::vector<LocalTrackRow> rows) {
                    auto* destination = tabForDocument(id);
                    if (destination == nullptr || rows.empty()) {
                        return;
                    }
                    if (action == LocalLibraryAction::replace) {
                        destination->model->replaceRows(std::move(rows), true);
                    } else {
                        destination->model->appendRows(std::move(rows), insertion);
                    }
                    markTabDirty(*destination);
                    syncArtwork(*destination);
                    schedulePersist();
                    tabs_->setCurrentWidget(destination->view);
                    // "Replace list and play", as this computer's library does.
                    if (action == LocalLibraryAction::replace &&
                        destination->model->rowCount() > 0) {
                        playRow(*destination, 0);
                    }
                });
        });
    connect(remote_library_, &LocalLibraryPanel::searchCommitted, this,
            [this](const QString& query, std::vector<LocalTrackRow> rows) {
                auto* destination = addListTab(
                    persistence::ListDocument{
                        .id = core::StableId::random(),
                        .kind = persistence::ListKind::scratch,
                        .name = utf8Bytes(QStringLiteral("Search: %1").arg(query)),
                        .pinned = false,
                        .dirty = false,
                        .items = {},
                        .remote = true},
                    true);
                destination->model->appendRows(std::move(rows));
                markTabDirty(*destination);
                syncArtwork(*destination);
            });
}

} // namespace trackknife::bench
