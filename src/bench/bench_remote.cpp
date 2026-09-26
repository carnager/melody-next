// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227: the remote engine beside this computer's. Its tabs list files on
// its machine and play there; its library sits beside this computer's in the
// source switch. Nothing here reads the remote files: what they are comes
// from the engine that has them.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/local_library_panel.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>

namespace trackknife::bench {

BenchMainWindow::EngineLink* BenchMainWindow::link(const EngineKey& key) const {
    const auto found = std::ranges::find(engines_, key, [](const auto& each) { return each->key; });
    return found != engines_.end() ? found->get() : nullptr;
}

BenchMainWindow::EngineLink* BenchMainWindow::linkOf(const EnginePlayback* playback) const {
    if (playback == nullptr) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(engines_, playback, [](const auto& each) { return each->playback; });
    return found != engines_.end() ? found->get() : nullptr;
}

EnginePlayback* BenchMainWindow::playbackOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->playback : nullptr;
}

CatalogueSource* BenchMainWindow::catalogueOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->catalogue.get() : nullptr;
}

LocalLibraryPanel* BenchMainWindow::libraryOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->library : nullptr;
}

BenchMainWindow::ListTab* BenchMainWindow::remoteQueueTab() {
    for (const auto& tab : list_tabs_) {
        if (!EngineKey::of(tab->document).isLocal()) {
            return tab.get();
        }
    }
    if (remoteCatalogue() == nullptr) {
        return nullptr;
    }
    // Opened on first connection, named after the remote so it reads as a
    // place rather than a list.
    auto* tab = addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                     .kind = persistence::ListKind::scratch,
                                                     .name = utf8Bytes(remoteCatalogue()->name()),
                                                     .pinned = false,
                                                     .dirty = false,
                                                     .items = {},
                                                     .engine = remoteEngine()->key.stored()},
                           false);
    schedulePersist();
    return tab;
}

void BenchMainWindow::connectRemoteEngine() {
    auto added = std::make_unique<EngineLink>();
    added->key = EngineKey::remote();
    added->catalogue =
        std::make_unique<CatalogueSource>(database_path_, CatalogueSource::Role::remote);
    if (!added->catalogue->configured()) {
        return;
    }
    added->playback = new EnginePlayback(*added->catalogue, this);
    engines_.push_back(std::move(added));
    if (list_sync_ != nullptr) {
        list_sync_->setEngine(EngineKey::remote(), remotePlayback());
        connect(remotePlayback(), &EnginePlayback::listChanged, this,
                [this](const QString& id, const quint64 revision, const bool deleted) {
                    list_sync_->listChanged(remotePlayback(), id, revision, deleted);
                    fetchEngineLists();
                });
        connect(remotePlayback(), &EnginePlayback::connected, this, [this] {
            list_sync_->reconnected(remotePlayback());
            flushEngineRelocations();
            fetchEngineLists();
        });
    }
    connect(remotePlayback(), &EnginePlayback::changed, this, [this] {
        followIfStartedElsewhere(remotePlayback());
        if (transport_ == remotePlayback()) {
            refreshTransport();
        }
    });
    connect(remotePlayback(), &EnginePlayback::ratingChanged, this,
            [this](const QString& hash, const unsigned rating) {
                adoptEngineRating(EngineKey::remote(), hash, rating);
            });
    connect(remotePlayback(), &EnginePlayback::failed, this, [this](const QString& message) {
        statusBar()->showMessage(QStringLiteral("Engine: %1").arg(message), 8'000);
    });
    const auto attached = [this] {
        // What it is doing now is not news; a start after this is.
        rememberEngineState(remotePlayback());
        // Connected, the remote says what it is called: the library tab shows
        // that rather than its address (an engine too old to say keeps it).
        static_cast<void>(remoteCatalogue()->open());
        for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
             ++index) {
            if (local_source_tabs_->tabData(index).toString() == QStringLiteral("remote")) {
                local_source_tabs_->setTabText(index, remoteCatalogue()->name());
            }
        }
        // The remote tabs were restored before there was a remote to ask for
        // their covers and missing tags -- or while it was away: they ask now.
        for (auto& tab : list_tabs_) {
            if (!EngineKey::of(tab->document).isLocal()) {
                // Named now that the remote has said its name.
                static_cast<ui::QueueTableView*>(tab->view)->setEmptyMessage(
                    emptyListTitle(EngineKey::of(tab->document)),
                    emptyListHint(EngineKey::of(tab->document)));
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
            const auto address = remoteCatalogue()->addressName();
            const auto announced = remoteCatalogue()->name();
            if (displayText(tab->document.name) == address && announced != address) {
                tab->document.name = utf8Bytes(announced);
                refreshTabChrome(*tab);
                schedulePersist();
            }
        }
        // Music the remote was already playing is followed, unless this
        // computer is playing: then that is what the transport shows, and
        // the remote waits until one of its tabs is played.
        const auto remote = remotePlayback()->state();
        const bool local_idle = localPlayback() == nullptr ||
                                localPlayback()->state().status == QStringLiteral("stopped");
        if (!remote.entry.isEmpty() && local_idle && transport_ != remotePlayback()) {
            followPlayback(remotePlayback());
        }
        if (transport_ == remotePlayback()) {
            reattachToEngine();
        }
    };
    connect(remotePlayback(), &EnginePlayback::connected, this, attached);
    if (remotePlayback()->active()) {
        attached();
    } else {
        // Still offered, so a remote that is down now has its tab to come
        // back to.
        static_cast<void>(remoteQueueTab());
    }

    remoteEngine()->library =
        new LocalLibraryPanel(*remoteCatalogue(), remoteEngine()->key, source_stack_);
    remoteLibrary()->setObjectName(QStringLiteral("bench-remote-library"));
    source_stack_->addWidget(remoteLibrary());
    // Ratings set in remote tabs are stored on the remote, and read from it.
    connect(remoteLibrary(), &LocalLibraryPanel::ratingsChanged, this,
            &BenchMainWindow::refreshLocalRatings);
    refreshLocalRatings();
    const auto index = local_source_tabs_->addTab(remoteCatalogue()->name());
    local_source_tabs_->setTabData(index, QStringLiteral("remote"));
    local_source_tabs_->setTabToolTip(index, remoteCatalogue()->describe());
    // Now that there is a remote to show instead, or it was the one chosen.
    applyLocalLibraryVisibility();
    selectPreferredSource();

    connect(
        remoteLibrary(), &LocalLibraryPanel::actionRequested, this,
        [this](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
            if (entries.empty()) {
                return;
            }
            if (action == LocalLibraryAction::request_next ||
                action == LocalLibraryAction::request_end) {
                remoteLibrary()->resolveEntryRows(
                    std::move(entries), [this, action](std::vector<LocalTrackRow> rows) {
                        enqueueLocalRequests(std::move(rows),
                                             action == LocalLibraryAction::request_next ? 0 : -1,
                                             remoteEngine()->key);
                    });
                return;
            }
            // Into the remote tab on screen, or the remote's own: a
            // remote file never lands in a local tab.
            auto* target = currentListTab();
            if (target == nullptr || EngineKey::of(target->document).isLocal()) {
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
                                              .engine = remoteEngine()->key.stored()},
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
            remoteLibrary()->resolveEntryRows(
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
    remoteLibrary()->setListTargets([this] { return listTargets(EngineKey::remote()); });
    connect(remoteLibrary(), &LocalLibraryPanel::addToListRequested, this,
            [this](std::vector<persistence::LibraryEntry> entries, const QString& id) {
                if (tabForDocument(id) == nullptr || entries.empty()) {
                    return;
                }
                remoteLibrary()->resolveEntryRows(
                    std::move(entries), [this, id](std::vector<LocalTrackRow> rows) {
                        auto* destination = tabForDocument(id);
                        if (destination == nullptr || rows.empty()) {
                            return;
                        }
                        const auto count = rows.size();
                        destination->model->appendRows(std::move(rows));
                        markTabDirty(*destination);
                        syncArtwork(*destination);
                        schedulePersist();
                        statusBar()->showMessage(
                            QStringLiteral("Added %1 to “%2”")
                                .arg(count == 1U ? QStringLiteral("1 track")
                                                 : QStringLiteral("%1 tracks").arg(count),
                                     displayText(destination->document.name)),
                            4'000);
                    });
            });
    connect(remoteLibrary(), &LocalLibraryPanel::searchCommitted, this,
            [this](const QString& query, std::vector<LocalTrackRow> rows) {
                auto* destination = addListTab(
                    persistence::ListDocument{
                        .id = core::StableId::random(),
                        .kind = persistence::ListKind::scratch,
                        .name = utf8Bytes(QStringLiteral("Search: %1").arg(query)),
                        .pinned = false,
                        .dirty = false,
                        .items = {},
                        .engine = remoteEngine()->key.stored()},
                    true);
                destination->model->appendRows(std::move(rows));
                markTabDirty(*destination);
                syncArtwork(*destination);
            });
}

} // namespace trackknife::bench
