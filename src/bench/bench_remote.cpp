// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227: the remote engine beside this computer's. Its tabs list files on
// its machine and play there; its library sits beside this computer's in the
// source switch. Nothing here reads the remote files: what they are comes
// from the engine that has them.

#include "bench/bench_main_window.hpp"
#include "bench/bench_main_window_helpers.hpp"
#include "bench/catalogue_source.hpp"
#include "bench/dynamic_playlist_dialog.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/remote_engines.hpp"
#include "bench/search_dialog.hpp"
#include "uicommon/queue_table_view.hpp"

#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>

namespace trackknife::bench {

// ADR-0234: an engine elsewhere has said who it is. Known until now by a
// placeholder -- the remote of an older release, or its address -- its
// lists and everything kept for it take its id. Known by another id, the
// address now leads to another engine: its link takes the new id, and the
// lists of the one it led to before stay that engine's.
void BenchMainWindow::adoptEngineIdentity(EngineLink& engine) {
    auto* remote = &engine;
    if (remote->catalogue == nullptr) {
        return;
    }
    const auto id = remote->catalogue->engineId();
    if (id.isEmpty() || id == remote->key.text()) {
        return;
    }
    const auto from = remote->key;
    const auto to = EngineKey::fromText(id);
    const bool lists_follow =
        from == EngineKey::remote() || from.text().startsWith(QStringLiteral("address:"));
    remote->key = to;
    remote->setting.id = id;
    rememberEngineId(remote->setting.address, id);
    if (remote->library != nullptr) {
        remote->library->setEngine(to);
    }
    for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
         ++index) {
        if (local_source_tabs_->tabData(index).toString() == from.text()) {
            local_source_tabs_->setTabData(index, to.text());
        }
    }
    if (!lists_follow) {
        if (list_sync_ != nullptr) {
            list_sync_->setEngine(from, nullptr);
            list_sync_->setEngine(to, remote->playback);
        }
        return;
    }
    for (auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == from) {
            tab->document.engine = to.stored();
            markViewEngine(tab->view, to);
        }
    }
    if (detached_playback_ && EngineKey::of(detached_playback_->document) == from) {
        detached_playback_->document.engine = to.stored();
    }
    if (list_sync_ != nullptr) {
        list_sync_->rekey(from, to);
    }
    if (up_next_engine_ == from) {
        up_next_engine_ = to;
        persistUpNext();
    }
    if (output_choices_engine_ == from) {
        output_choices_engine_ = to;
    }
    // Dialogs that offer the libraries by key are made again when next opened.
    if (auto* dialog = findChild<DynamicPlaylistDialog*>()) {
        dialog->close();
    }
    if (search_dialog_ != nullptr) {
        search_dialog_->close();
    }
    schedulePersist();
}

BenchMainWindow::EngineLink* BenchMainWindow::linkOf(const EnginePlayback* playback) const {
    if (playback == nullptr) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(engines_, playback, [](const auto& each) { return each->playback; });
    return found != engines_.end() ? found->get() : nullptr;
}

LocalLibraryPanel* BenchMainWindow::libraryOf(const EngineKey& key) const {
    const auto* engine = link(key);
    return engine != nullptr ? engine->library : nullptr;
}

BenchMainWindow::ListTab* BenchMainWindow::remoteQueueTab() {
    auto* remote = remoteEngine();
    return remote != nullptr ? engineTab(*remote) : nullptr;
}

BenchMainWindow::ListTab* BenchMainWindow::engineTab(EngineLink& engine) {
    // Its own, by its key: a list of an engine the address led to before
    // is not its tab.
    for (const auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == engine.key) {
            return tab.get();
        }
    }
    if (engine.catalogue == nullptr) {
        return nullptr;
    }
    // Opened on first connection, named after the engine so it reads as a
    // place rather than a list.
    auto* tab = addListTab(persistence::ListDocument{.id = core::StableId::random(),
                                                     .kind = persistence::ListKind::scratch,
                                                     .name = utf8Bytes(engine.catalogue->name()),
                                                     .pinned = false,
                                                     .dirty = false,
                                                     .items = {},
                                                     .engine = engine.key.stored()},
                           false);
    schedulePersist();
    return tab;
}

void BenchMainWindow::syncRemoteEngines() {
    const auto wanted = loadRemoteEngines();
    const auto listed = [&wanted](const EngineLink& engine) {
        return std::ranges::any_of(wanted, [&engine](const RemoteEngineSetting& setting) {
            return setting.address == engine.setting.address &&
                   setting.effectivePassword() == engine.password;
        });
    };
    std::vector<EngineKey> gone;
    for (const auto& engine : engines_) {
        if (!engine->key.isLocal() && !listed(*engine)) {
            gone.push_back(engine->key);
        }
    }
    for (const auto& key : gone) {
        disconnectEngine(key);
    }
    for (const auto& setting : wanted) {
        const bool connected = std::ranges::any_of(engines_, [&setting](const auto& engine) {
            return !engine->key.isLocal() && engine->setting.address == setting.address;
        });
        if (!connected) {
            // An engine added now is new to this window: it has no lists of
            // an older release to be the owner of.
            connectRemoteEngine(setting, false);
        }
    }
    // What mounts and names say now.
    for (auto& engine : engines_) {
        for (const auto& setting : wanted) {
            if (!engine->key.isLocal() && setting.address == engine->setting.address) {
                engine->setting.music_folder = setting.music_folder;
                engine->setting.reachable_at = setting.reachable_at;
            }
        }
    }
    keepTabGroupsTogether();
    fetchEngineLists();
    refreshListsPanel();
}

void BenchMainWindow::disconnectEngine(const EngineKey& key) {
    const auto found =
        std::ranges::find(engines_, key, [](const auto& engine) { return engine->key; });
    if (found == engines_.end() || key.isLocal()) {
        return;
    }
    auto& engine = **found;
    // Nothing is to follow or play it any more.
    if (transport_ == engine.playback) {
        transport_ = localPlayback();
        playback_.anchors = {};
        playback_.row = -1;
        refreshTransport();
    }
    if (list_sync_ != nullptr) {
        list_sync_->setEngine(key, nullptr);
    }
    // Dialogs that offer the libraries by engine are made again when next
    // opened.
    if (auto* dialog = findChild<DynamicPlaylistDialog*>()) {
        dialog->close();
    }
    if (search_dialog_ != nullptr) {
        search_dialog_->close();
    }
    for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
         ++index) {
        if (local_source_tabs_->tabData(index).toString() == key.text()) {
            local_source_tabs_->removeTab(index);
            break;
        }
    }
    // The connection first -- it waits for what it has in hand -- then the
    // panel, then the catalogue both of them read through.
    if (engine.playback != nullptr) {
        engine.playback->retire();
        disconnect(engine.playback, nullptr, this, nullptr);
        delete engine.playback;
        engine.playback = nullptr;
    }
    if (engine.library != nullptr) {
        disconnect(engine.library, nullptr, this, nullptr);
        source_stack_->removeWidget(engine.library);
        delete engine.library;
        engine.library = nullptr;
    }
    engines_.erase(found);
    selectPreferredSource();
    refreshActiveContext();
    refreshTransport();
}

void BenchMainWindow::connectRemoteEngine(const RemoteEngineSetting& setting, const bool first) {
    auto added = std::make_unique<EngineLink>();
    // ADR-0234: by the id it gave when last reached. Until it has been, a
    // placeholder: "remote" for the one remote of an older release, whose
    // lists say so, or its address for one added since.
    added->key = !setting.id.isEmpty() ? EngineKey::fromText(setting.id)
                 : first ? EngineKey::remote()
                         : EngineKey::fromText(QStringLiteral("address:") + setting.address);
    added->setting = setting;
    added->password = setting.effectivePassword();
    added->catalogue =
        std::make_unique<CatalogueSource>(database_path_, setting.address, added->password);
    if (!added->catalogue->configured()) {
        return;
    }
    added->playback = new EnginePlayback(*added->catalogue, this);
    auto* link = added.get();
    engines_.push_back(std::move(added));
    watchFileWork(*link);
    if (list_sync_ != nullptr) {
        list_sync_->setEngine(link->key, link->playback);
        connect(link->playback, &EnginePlayback::listChanged, this,
                [this, link](const QString& id, const quint64 revision, const bool deleted) {
                    list_sync_->listChanged(link->playback, id, revision, deleted);
                    fetchEngineLists();
                });
        connect(link->playback, &EnginePlayback::connected, this, [this, link] {
            list_sync_->reconnected(link->playback);
            flushEngineRelocations();
            fetchEngineLists();
        });
    }
    connect(link->playback, &EnginePlayback::changed, this, [this, link] {
        followIfStartedElsewhere(link->playback);
        if (transport_ == link->playback) {
            refreshTransport();
        }
    });
    connect(link->playback, &EnginePlayback::ratingChanged, this,
            [this, link](const QString& hash, const unsigned rating) {
                adoptEngineRating(link->key, hash, rating);
            });
    connect(link->playback, &EnginePlayback::failed, this, [this, link](const QString& message) {
        statusBar()->showMessage(QStringLiteral("Engine: %1").arg(message), 8'000);
    });
    const auto attached = [this, link] {
        // What it is doing now is not news; a start after this is.
        rememberEngineState(link->playback);
        // Connected, the remote says what it is called: the library tab shows
        // that rather than its address (an engine too old to say keeps it).
        static_cast<void>(link->catalogue->open());
        adoptEngineIdentity(*link);
        for (int index = 0; local_source_tabs_ != nullptr && index < local_source_tabs_->count();
             ++index) {
            if (local_source_tabs_->tabData(index).toString() == link->key.text()) {
                local_source_tabs_->setTabText(index, link->catalogue->name());
            }
        }
        // The remote tabs were restored before there was a remote to ask for
        // their covers and missing tags -- or while it was away: they ask now.
        for (auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == link->key) {
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
        if (auto* tab = engineTab(*link); tab != nullptr) {
            const auto address = link->catalogue->addressName();
            const auto announced = link->catalogue->name();
            if (displayText(tab->document.name) == address && announced != address) {
                tab->document.name = utf8Bytes(announced);
                refreshTabChrome(*tab);
                schedulePersist();
            }
        }
        // Music the remote was already playing is followed, unless this
        // computer is playing: then that is what the transport shows, and
        // the remote waits until one of its tabs is played.
        const auto remote = link->playback->state();
        const bool local_idle = localPlayback() == nullptr ||
                                localPlayback()->state().status == QStringLiteral("stopped");
        if (!remote.entry.isEmpty() && local_idle && transport_ != link->playback) {
            followPlayback(link->playback);
        }
        if (transport_ == link->playback) {
            reattachToEngine();
        }
    };
    connect(link->playback, &EnginePlayback::connected, this, attached);
    if (link->playback->active()) {
        attached();
    } else {
        // Still offered, so a remote that is down now has its tab to come
        // back to.
        static_cast<void>(engineTab(*link));
    }

    link->library = new LocalLibraryPanel(*link->catalogue, link->key, source_stack_);
    link->library->setObjectName(
        first ? QStringLiteral("bench-remote-library")
              : QStringLiteral("bench-remote-library-%1").arg(engines_.size() - 1));
    source_stack_->addWidget(link->library);
    // Ratings set in remote tabs are stored on the remote, and read from it.
    connect(link->library, &LocalLibraryPanel::ratingsChanged, this,
            &BenchMainWindow::refreshLocalRatings);
    refreshLocalRatings();
    const auto index = local_source_tabs_->addTab(link->catalogue->name());
    // The engine's key, which the tab is found and followed by (ADR-0234).
    local_source_tabs_->setTabData(index, link->key.text());
    local_source_tabs_->setTabToolTip(index, link->catalogue->describe());
    // Now that there is a remote to show instead, or it was the one chosen.
    applyLocalLibraryVisibility();
    selectPreferredSource();

    connect(
        link->library, &LocalLibraryPanel::actionRequested, this,
        [this, link](std::vector<persistence::LibraryEntry> entries, LocalLibraryAction action) {
            if (entries.empty()) {
                return;
            }
            if (action == LocalLibraryAction::request_next ||
                action == LocalLibraryAction::request_end) {
                link->library->resolveEntryRows(
                    std::move(entries), [this, link, action](std::vector<LocalTrackRow> rows) {
                        enqueueLocalRequests(std::move(rows),
                                             action == LocalLibraryAction::request_next ? 0 : -1,
                                             link->key);
                    });
                return;
            }
            // Into the remote tab on screen, or the remote's own: a
            // remote file never lands in a local tab.
            auto* target = currentListTab();
            if (target == nullptr || EngineKey::of(target->document).isLocal()) {
                target = engineTab(*link);
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
                                              .engine = link->key.stored()},
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
            link->library->resolveEntryRows(
                std::move(entries),
                [this, link, id, action, insertion](std::vector<LocalTrackRow> rows) {
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
    link->library->setListTargets([this, link] { return listTargets(link->key); });
    connect(link->library, &LocalLibraryPanel::addToListRequested, this,
            [this, link](std::vector<persistence::LibraryEntry> entries, const QString& id) {
                if (tabForDocument(id) == nullptr || entries.empty()) {
                    return;
                }
                link->library->resolveEntryRows(
                    std::move(entries), [this, link, id](std::vector<LocalTrackRow> rows) {
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
    connect(link->library, &LocalLibraryPanel::searchCommitted, this,
            [this, link](const QString& query, std::vector<LocalTrackRow> rows) {
                auto* destination = addListTab(
                    persistence::ListDocument{
                        .id = core::StableId::random(),
                        .kind = persistence::ListKind::scratch,
                        .name = utf8Bytes(QStringLiteral("Search: %1").arg(query)),
                        .pinned = false,
                        .dirty = false,
                        .items = {},
                        .engine = link->key.stored()},
                    true);
                destination->model->appendRows(std::move(rows));
                markTabDirty(*destination);
                syncArtwork(*destination);
            });
}

} // namespace trackknife::bench
