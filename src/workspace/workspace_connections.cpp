// SPDX-License-Identifier: GPL-3.0-only

// ADR-0227, ADR-0234: the engines the workspace reaches -- this computer's,
// always, and those configured in Settings -- connected, followed as they
// come and go, and let go of. Nothing here reads the remote files: what they
// are comes from the engine that has them.

#include "workspace/workspace.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/post_back.hpp"
#include "bench/remote_engines.hpp"
#include "bench/remote_mount.hpp"
#include "bench/settings_keys.hpp"
#include "workspace/workspace_view.hpp"

#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

void Workspace::connectLocalEngine() {
    // Built once, before anything that needs a catalogue: the panel, the
    // search dialog and dynamic playlists all take this rather than a path.
    localEngine().catalogue =
        std::make_unique<CatalogueSource>(database_path_, CatalogueSource::Role::local);
    // Its own connection: the engine serves one connection in order, so a
    // transport command behind a library query would wait for it.
    localEngine().playback = new EnginePlayback(*localEngine().catalogue, this);
    watchFileWork(localEngine());
    transport_ = localPlayback();
    connect(localPlayback(), &EnginePlayback::changed, this, [this] {
        followIfStartedElsewhere(localPlayback());
        if (transport_ == localPlayback()) {
            view_->refreshTransport();
        }
        // An outdated engine left alone while it played is renewed once it
        // has stopped.
        if (engine_renewal_pending_ &&
            localPlayback()->state().status != QStringLiteral("playing")) {
            QTimer::singleShot(0, this, [this] { renewOutdatedLocalEngine(); });
        }
    });
    connect(localPlayback(), &EnginePlayback::ratingChanged, this,
            [this](const QString& hash, const unsigned rating) {
                adoptEngineRating(EngineKey::local(), hash, rating);
            });
    connect(localPlayback(), &EnginePlayback::failed, this, [this](const QString& message) {
        view_->showMessage(QStringLiteral("Engine: %1").arg(message), 8'000);
    });
    list_sync_ = new EngineListSync(this);
    list_sync_->setEngine(EngineKey::local(), localPlayback());
    connect(list_sync_, &EngineListSync::adopted, this, &Workspace::adoptEngineList);
    connect(list_sync_, &EngineListSync::wantsSave, this, &Workspace::schedulePersist);
    connect(localPlayback(), &EnginePlayback::listChanged, this,
            [this](const QString& id, const quint64 revision, const bool deleted) {
                list_sync_->listChanged(localPlayback(), id, revision, deleted);
                view_->engineListsChanged();
            });
    loadPendingRelocations();
    connect(localPlayback(), &EnginePlayback::connected, this, [this] {
        // A new engine, or this one restarted: compared again, and given its
        // lists -- and any moves it missed.
        list_sync_->reconnected(localPlayback());
        flushEngineRelocations();
        view_->engineListsChanged();
        QTimer::singleShot(0, this, [this] { renewOutdatedLocalEngine(); });
        // What it is doing now is not news; a start after this is.
        rememberEngineState(localPlayback());
        if (transport_ != localPlayback()) {
            return;
        }
        // A reconnection is a new engine as far as it is concerned: it knows
        // none of this window's settings, and it may already be playing.
        applyLocalPlaybackModes();
        reattachToEngine();
    });
    if (localPlayback()->active()) {
        QTimer::singleShot(0, this, [this] { renewOutdatedLocalEngine(); });
        // An engine starts with its own defaults and has never heard of this
        // window's settings, so they are handed over the moment the connection
        // exists. This runs after the transport is built, which is why sending
        // them there reached nothing and the first track played with no gain
        // applied until a mode was toggled.
        applyLocalPlaybackModes();
    }
}

// ADR-0234: an engine elsewhere has said who it is. Known until now by a
// placeholder -- the remote of an older release, or its address -- its
// lists and everything kept for it take its id. Known by another id, the
// address now leads to another engine: its link takes the new id, and the
// lists of the one it led to before stay that engine's.
void Workspace::adoptEngineIdentity(EngineLink& engine) {
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
    if (!lists_follow) {
        if (list_sync_ != nullptr) {
            list_sync_->setEngine(from, nullptr);
            list_sync_->setEngine(to, remote->playback);
        }
        view_->engineRekeyed(*remote, from, false);
        return;
    }
    for (auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == from) {
            tab->document.engine = to.stored();
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
    view_->engineRekeyed(*remote, from, true);
    schedulePersist();
}

void Workspace::syncRemoteEngines() {
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
    view_->enginesSynced();
    view_->engineListsChanged();
}

void Workspace::disconnectEngine(const EngineKey& key) {
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
        view_->refreshTransport();
    }
    if (list_sync_ != nullptr) {
        list_sync_->setEngine(key, nullptr);
    }
    // The connection first -- it waits for what it has in hand -- then what
    // shows it, then the catalogue both of them read through.
    if (engine.playback != nullptr) {
        engine.playback->retire();
        disconnect(engine.playback, nullptr, this, nullptr);
        delete engine.playback;
        engine.playback = nullptr;
    }
    view_->engineRemoving(engine);
    engines_.erase(found);
    view_->engineRemoved();
    view_->refreshTransport();
}

void Workspace::connectRemoteEngine(const RemoteEngineSetting& setting, const bool first) {
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
                    view_->engineListsChanged();
                });
        connect(link->playback, &EnginePlayback::connected, this, [this, link] {
            list_sync_->reconnected(link->playback);
            flushEngineRelocations();
            view_->engineListsChanged();
        });
    }
    connect(link->playback, &EnginePlayback::changed, this, [this, link] {
        followIfStartedElsewhere(link->playback);
        if (transport_ == link->playback) {
            view_->refreshTransport();
        }
    });
    connect(link->playback, &EnginePlayback::ratingChanged, this,
            [this, link](const QString& hash, const unsigned rating) {
                adoptEngineRating(link->key, hash, rating);
            });
    connect(link->playback, &EnginePlayback::failed, this, [this](const QString& message) {
        view_->showMessage(QStringLiteral("Engine: %1").arg(message), 8'000);
    });
    const auto attached = [this, link] {
        // What it is doing now is not news; a start after this is.
        rememberEngineState(link->playback);
        // Connected, the remote says what it is called: what shows it says
        // that rather than its address (an engine too old to say keeps it).
        static_cast<void>(link->catalogue->open());
        adoptEngineIdentity(*link);
        view_->engineAttached(*link);
        // The remote tabs were restored before there was a remote to ask for
        // their covers and missing tags -- or while it was away: they ask now.
        for (auto& tab : list_tabs_) {
            if (EngineKey::of(tab->document) == link->key) {
                enqueueUnprobedRows(*tab);
                syncArtwork(*tab);
            }
        }
        refreshRatings();
        // A remote that restarted holds the Up Next it saved; this window's
        // is the one the user sees, so it is stated again.
        engine_requests_.reset();
        syncEngineRequests();
        // Its tab, named after it -- by address if it was made while the
        // remote was away, which is renamed now that it has said its name.
        // A name someone chose is theirs, and kept.
        if (auto* tab = engineTab(*link); tab != nullptr) {
            const auto address = link->catalogue->addressName();
            const auto announced = link->catalogue->name();
            if (displayText(tab->document.name) == address && announced != address) {
                tab->document.name = utf8Bytes(announced);
                view_->refreshTabChrome(*tab);
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
    view_->engineConnected(*link, first);
}

void Workspace::adoptEngineRating(const EngineKey& engine, const QString& hash,
                                  const unsigned rating) {
    const QHash<QString, unsigned> ratings{{hash, rating}};
    for (const auto& tab : list_tabs_) {
        if (EngineKey::of(tab->document) == engine) {
            tab->model->applyRatings(ratings);
        }
    }
    view_->engineRatingsChanged(engine, ratings);
}

void Workspace::watchFileWork(EngineLink& link) {
    if (link.playback == nullptr) {
        return;
    }
    const auto probe = [this, &link] {
        if (!link.catalogue || !link.catalogue->endpoint()) {
            return;
        }
        // A fresh connection each time the engine connects: one restarted as
        // a newer engine is asked again, not remembered as it was.
        auto work = std::make_shared<engine::RemoteFileWork>(*link.catalogue->endpoint());
        link.file_work = work;
        link.does_file_work = false;
        const QPointer workspace{this};
        static_cast<void>(QtConcurrent::run([workspace, work] {
            const bool does = work->supported();
            std::vector<EngineInterruption> interrupted;
            std::size_t recovered = 0U;
            if (does) {
                // ADR-0237: the engine looks things up with the key kept in
                // Settings; handed over whenever it connects.
                const auto key = QSettings{}
                                     .value(QLatin1String(SettingsKeys::acoustid_client_key))
                                     .toString()
                                     .trimmed();
                if (!key.isEmpty()) {
                    static_cast<void>(work->set_acoustid_key(key.toStdString()));
                }
                // Once chosen in Settings, whether ratings go into the files
                // is the same on every engine this window reaches.
                const QSettings chosen;
                if (chosen.contains(QLatin1String(SettingsKeys::ratings_in_tags_key))) {
                    static_cast<void>(work->set_rating_tags(
                        chosen.value(QLatin1String(SettingsKeys::ratings_in_tags_key)).toBool()));
                }
                if (chosen.contains(QLatin1String(SettingsKeys::rating_tag_scale_key))) {
                    static_cast<void>(work->set_rating_scale(
                        chosen.value(QLatin1String(SettingsKeys::rating_tag_scale_key))
                            .toString()
                            .toStdString()));
                }
                if (auto answer = work->interrupted()) {
                    // What it finished or rolled back at its start, itself.
                    recovered = answer->value("recovered", std::size_t{0U});
                    for (const auto& entry :
                         answer->value("interrupted", protocol::Json::array())) {
                        auto id = core::StableId::parse(entry.value("id", std::string{}));
                        auto path = protocol::decode_raw_path(entry.value("path", std::string{}));
                        if (!id || !path) {
                            continue;
                        }
                        const auto message = entry.find("message");
                        const bool has_message = message != entry.end() && message->is_string();
                        // A move names where it was going, as this window's
                        // own interrupted moves do.
                        std::optional<std::string> target;
                        if (const auto encoded = entry.value("target", std::string{});
                            !encoded.empty()) {
                            if (auto decoded = protocol::decode_raw_path(encoded)) {
                                target = std::move(*decoded);
                            }
                        }
                        QString detail;
                        if (has_message) {
                            detail = QString::fromStdString(message->get<std::string>());
                        } else if (target) {
                            detail = QStringLiteral("An interrupted move could not be finished or "
                                                    "safely rolled back");
                        } else {
                            detail = QStringLiteral("An interrupted tag write could not be "
                                                    "finished or safely rolled back; the file "
                                                    "was left untouched");
                        }
                        if (target) {
                            detail +=
                                QStringLiteral(" · planned target %1")
                                    .arg(QString::fromStdString(core::display_raw_path(*target)));
                        }
                        interrupted.push_back(EngineInterruption{.id = *id,
                                                                 .raw_path = std::move(*path),
                                                                 .detail = std::move(detail),
                                                                 .move = target.has_value()});
                    }
                }
            }
            postBack(workspace, [workspace, work, does, recovered,
                                 interrupted = std::move(interrupted)] {
                if (!workspace) {
                    return;
                }
                if (recovered > 0U) {
                    workspace->view_->showMessage(
                        QStringLiteral("Recovered %1 interrupted file operation%2")
                            .arg(recovered)
                            .arg(recovered == 1U ? QString{} : QStringLiteral("s")),
                        5'000);
                }
                for (const auto& reported : interrupted) {
                    if (std::ranges::none_of(
                            workspace->engine_interruptions_,
                            [&reported](const auto& known) { return known.id == reported.id; })) {
                        workspace->engine_interruptions_.push_back(reported);
                    }
                }
                workspace->view_->engineInterruptionsChanged(!interrupted.empty());
                // By the connection, not the key: an engine's key changes
                // when it first says its id.
                for (const auto& candidate : workspace->engines_) {
                    if (candidate->file_work == work) {
                        candidate->does_file_work = does;
                    }
                }
                if (does) {
                    // ADR-0237: it names files with this window's layouts.
                    workspace->pushLayouts();
                }
            });
        }));
    };
    connect(link.playback, &EnginePlayback::connected, this, probe);
    if (link.playback->active()) {
        probe();
    }
}

void Workspace::pushLayouts(std::vector<core::StableId> removed) {
    if (persistence_ == nullptr) {
        return;
    }
    const QPointer workspace{this};
    persistence_->loadOutputProfiles(
        [workspace, removed = std::move(removed)](
            std::vector<persistence::SavedOutputLayoutProfile> layouts, auto, QString error) {
            if (!workspace || !error.isEmpty()) {
                return;
            }
            for (const auto& engine : workspace->engines_) {
                // This computer's engine keeps its layouts in this workspace:
                // they are these, and a copy sent to it could only be older.
                if (engine->key.isLocal() || !engine->does_file_work || !engine->file_work) {
                    continue;
                }
                // In the order they were made: one thread, so an older set
                // never lands after a newer one.
                static_cast<void>(QtConcurrent::run(
                    &workspace->layout_pushes_, [work = engine->file_work, layouts, removed] {
                        static_cast<void>(work->put_layouts(layouts, removed));
                    }));
            }
        });
}

namespace {

constexpr auto pending_relocations_key = "lists/pending-relocations";

} // namespace

void Workspace::queueEngineRelocation(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) {
        return;
    }
    pending_relocations_.push_back(PendingRelocation{.from = from, .to = to, .done = {}});
    storePendingRelocations();
    flushEngineRelocations();
}

void Workspace::storePendingRelocations() const {
    auto pending = protocol::Json::array();
    for (const auto& move : pending_relocations_) {
        pending.push_back(protocol::Json{{"from", protocol::encode_raw_path(move.from)},
                                         {"to", protocol::encode_raw_path(move.to)},
                                         {"done", [&move] {
                                              auto done = protocol::Json::array();
                                              for (const auto& engine : move.done) {
                                                  done.push_back(engine.toStdString());
                                              }
                                              return done;
                                          }()}});
    }
    QSettings settings;
    if (pending_relocations_.empty()) {
        settings.remove(QLatin1String(pending_relocations_key));
    } else {
        settings.setValue(QLatin1String(pending_relocations_key),
                          QString::fromStdString(pending.dump()));
    }
}

void Workspace::loadPendingRelocations() {
    const auto stored =
        QSettings{}.value(QLatin1String(pending_relocations_key)).toString().toStdString();
    const auto parsed = protocol::Json::parse(stored, nullptr, false);
    if (!parsed.is_array()) {
        return;
    }
    for (const auto& move : parsed) {
        auto from = protocol::decode_raw_path(move.value("from", std::string{}));
        auto to = protocol::decode_raw_path(move.value("to", std::string{}));
        if (from && to) {
            PendingRelocation pending{.from = std::move(*from), .to = std::move(*to), .done = {}};
            for (const auto& engine : move.value("done", protocol::Json::array())) {
                if (engine.is_string()) {
                    pending.done.insert(QString::fromStdString(engine.get<std::string>()));
                }
            }
            // An older release said only whether this computer's and the
            // remote had it.
            if (move.value("local", false)) {
                pending.done.insert(EngineKey::local().text());
            }
            if (move.value("remote", false)) {
                pending.done.insert(QStringLiteral("*"));
            }
            pending_relocations_.push_back(std::move(pending));
        }
    }
}

void Workspace::flushEngineRelocations() {
    // What an engine calls a path here: this computer's the same; another's
    // through its mount, or the same when its music is at the same place
    // here. A file outside its mount is not its, and there is nothing to
    // tell it.
    const auto path_for = [this](const EngineLink& engine) {
        return [mount = mountOf(engine), local = engine.key.isLocal()](
                   const std::string& path) -> std::optional<std::string> {
            if (local || mount.local_folder.empty()) {
                return path;
            }
            if (!path_within(path, mount.local_folder)) {
                return std::nullopt;
            }
            return mount.remote_folder + path.substr(mount.local_folder.size());
        };
    };
    for (auto& move : pending_relocations_) {
        for (const auto& engine : engines_) {
            const auto mapped = path_for(*engine);
            if (!engine->key.isLocal() && !mapped(move.from) && !mapped(move.to)) {
                move.done.insert(engine->key.text());
            }
        }
    }
    // Done once every engine this window reaches has it.
    const auto finished = [this](const PendingRelocation& move) {
        return std::ranges::all_of(
            engines_, [&move](const auto& engine) { return move.doneFor(engine->key); });
    };
    std::erase_if(pending_relocations_, finished);
    storePendingRelocations();
    for (const auto& engine : engines_) {
        auto* playback = engine->playback;
        if (playback == nullptr || !playback->active() || engine->relocating) {
            continue;
        }
        const auto mapped = path_for(*engine);
        auto moves = protocol::Json::array();
        std::vector<std::pair<std::string, std::string>> sent;
        for (const auto& move : pending_relocations_) {
            if (move.doneFor(engine->key)) {
                continue;
            }
            const auto from = mapped(move.from);
            const auto to = mapped(move.to);
            if (!from || !to) {
                continue;
            }
            moves.push_back(protocol::Json{{"from", protocol::encode_raw_path(*from)},
                                           {"to", protocol::encode_raw_path(*to)}});
            sent.emplace_back(move.from, move.to);
        }
        if (sent.empty()) {
            continue;
        }
        engine->relocating = true;
        playback->request(
            QStringLiteral("list.relocate"), protocol::Json{{"moves", std::move(moves)}},
            [this, key = engine->key, sent, finished](const core::Result<protocol::Json>& answer) {
                if (auto* relocated = link(key); relocated != nullptr) {
                    relocated->relocating = false;
                }
                // An engine that predates lists has nothing to follow: done.
                if (!answer && answer.error().code != core::ErrorCode::unsupported) {
                    return;
                }
                for (auto& move : pending_relocations_) {
                    if (std::ranges::find(sent, std::pair{move.from, move.to}) != sent.end()) {
                        move.done.insert(key.text());
                    }
                }
                std::erase_if(pending_relocations_, finished);
                storePendingRelocations();
                flushEngineRelocations();
            });
    }
}

} // namespace trackknife::bench
