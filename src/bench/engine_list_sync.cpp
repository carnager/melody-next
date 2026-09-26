// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_list_sync.hpp"

#include "bench/engine_playback.hpp"
#include "trackknife/protocol/message.hpp"

#include <QSettings>

#include <functional>
#include <unordered_set>
#include <utility>

namespace trackknife::bench {
namespace {

using protocol::Json;

// The first value of a snapshot field, as the tab shows it.
[[nodiscard]] std::string field(const persistence::ListItem& item, const std::string_view name) {
    for (const auto& snapshot : item.fields) {
        if (snapshot.name == name) {
            return snapshot.value;
        }
    }
    return {};
}

// What an engine keeps of a list, so a change to anything else -- a cached
// tag the engine does not hold -- is not sent.
[[nodiscard]] std::size_t fingerprint(const persistence::ListDocument& document) {
    std::string text = document.name;
    text += document.kind == persistence::ListKind::saved ? "\x01" : "\x02";
    for (const auto& item : document.items) {
        text += item.entry_id.to_string();
        text += '\0';
        text += item.source_reference;
        text += '\0';
        text += item.logical_reference.value_or(std::string{});
        text += '\0';
        if (item.segment) {
            text += std::to_string(item.segment->start_sample) + ':' +
                    std::to_string(item.segment->end_sample.value_or(-1));
        }
        if (item.source_selection) {
            text += '/' + std::to_string(item.source_selection->audio_stream_index.value_or(-1)) +
                    '/' + std::to_string(item.source_selection->subsong_index.value_or(-1));
        }
        text += std::to_string(item.duration_ms.value_or(-1));
        text += field(item, "title") + '\0' + field(item, "artist") + '\0' + field(item, "album");
        text += '\n';
    }
    return std::hash<std::string>{}(text);
}

[[nodiscard]] Json item_json(const persistence::ListItem& item) {
    Json rendered{{"entry", item.entry_id.to_string()},
                  {"path", protocol::encode_raw_path(item.source_reference)},
                  {"title", protocol::displayable_text(field(item, "title"))},
                  {"artist", protocol::displayable_text(field(item, "artist"))},
                  {"album", protocol::displayable_text(field(item, "album"))}};
    if (item.logical_reference) {
        rendered["logical"] = protocol::encode_raw_path(*item.logical_reference);
    }
    if (item.segment) {
        rendered["segment"] = Json{{"start_sample", item.segment->start_sample}};
        if (item.segment->end_sample) {
            rendered["segment"]["end_sample"] = *item.segment->end_sample;
        }
    }
    if (item.source_selection) {
        Json selection = Json::object();
        if (item.source_selection->audio_stream_index) {
            selection["stream_index"] = *item.source_selection->audio_stream_index;
        }
        if (item.source_selection->subsong_index) {
            selection["subsong_index"] = *item.source_selection->subsong_index;
        }
        rendered["selection"] = std::move(selection);
    }
    if (item.duration_ms && *item.duration_ms >= 0) {
        rendered["duration_ms"] = *item.duration_ms;
    }
    return rendered;
}

[[nodiscard]] std::optional<int> optional_int(const Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? std::optional{found->get<int>()}
                                                               : std::nullopt;
}

// A list as list.get answers it, in the window's terms: what it would have
// saved itself for the same rows. An entry the engine could not describe is
// left out.
[[nodiscard]] std::optional<persistence::ListDocument> document_from(const Json& answer,
                                                                     const EngineKey& engine) {
    auto id = core::StableId::parse(answer.value("id", std::string{}));
    if (!id) {
        return std::nullopt;
    }
    persistence::ListDocument document{.id = *id,
                                       .kind = answer.value("kind", std::string{}) == "saved"
                                                   ? persistence::ListKind::saved
                                                   : persistence::ListKind::scratch,
                                       .name = answer.value("name", std::string{}),
                                       .pinned = false,
                                       .dirty = false,
                                       .items = {},
                                       .remote = !engine.isLocal()};
    for (const auto& value : answer.value("items", Json::array())) {
        auto path = protocol::decode_raw_path(value.value("path", std::string{}));
        auto entry = core::StableId::parse(value.value("entry", std::string{}));
        if (!path || !entry) {
            continue;
        }
        persistence::ListItem item{.entry_id = *entry,
                                   .source = persistence::ListSource::local,
                                   .profile_id = std::nullopt,
                                   .source_reference = std::move(*path),
                                   .logical_reference = std::nullopt,
                                   .segment = std::nullopt,
                                   .source_selection = std::nullopt,
                                   .duration_ms = std::nullopt,
                                   .source_revision = std::nullopt,
                                   .fields = {}};
        if (const auto logical = value.find("logical");
            logical != value.end() && logical->is_string()) {
            if (auto decoded = protocol::decode_raw_path(logical->get<std::string>())) {
                item.logical_reference = std::move(*decoded);
            }
        }
        if (const auto segment = value.find("segment");
            segment != value.end() && segment->is_object()) {
            const auto end = segment->find("end_sample");
            item.segment = persistence::ListItemSegment{
                .start_sample = segment->value("start_sample", std::int64_t{0}),
                .end_sample = end != segment->end() && end->is_number_integer()
                                  ? std::optional{end->get<std::int64_t>()}
                                  : std::nullopt};
        }
        if (const auto selection = value.find("selection");
            selection != value.end() && selection->is_object()) {
            persistence::ListItemSourceSelection chosen{
                .audio_stream_index = optional_int(*selection, "stream_index"),
                .subsong_index = optional_int(*selection, "subsong_index")};
            if (chosen.audio_stream_index || chosen.subsong_index) {
                item.source_selection = chosen;
            }
        }
        if (const auto duration = value.find("duration_ms");
            duration != value.end() && duration->is_number_integer()) {
            item.duration_ms = duration->get<std::int64_t>();
        }
        for (const auto* name : {"title", "artist", "album"}) {
            auto text = value.value(name, std::string{});
            if (!text.empty()) {
                item.fields.push_back({.name = name, .value = std::move(text)});
            }
        }
        document.items.push_back(std::move(item));
    }
    return document;
}

} // namespace

EngineListSync::EngineListSync(QObject* parent) : QObject(parent) {
    for (const auto& pending : QSettings{}.value(QLatin1String(removals_key)).toStringList()) {
        // "engine<TAB>id"; before engines had keys, "r:id" or "l:id".
        if (const auto tab = pending.indexOf(QLatin1Char('\t')); tab > 0) {
            removals_.emplace(EngineKey::fromText(pending.left(tab)),
                              pending.mid(tab + 1).toStdString());
        } else if (pending.size() > 2 && pending.at(1) == QLatin1Char(':')) {
            removals_.emplace(pending.front() == QLatin1Char('r') ? EngineKey::remote()
                                                                  : EngineKey::local(),
                              pending.mid(2).toStdString());
        }
    }
}

std::optional<persistence::ListDocument>
EngineListSync::documentFromAnswer(const protocol::Json& answer, const EngineKey& engine) {
    return document_from(answer, engine);
}

void EngineListSync::opened(const persistence::ListDocument& document,
                            const std::uint64_t revision) {
    auto& known = known_[document.id.to_string()];
    known.working = document.kind != persistence::ListKind::saved;
    known.engine = EngineKey::of(document);
    known.dirty = false;
    known.fingerprint = fingerprint(document);
    known.local = known.fingerprint;
    known.revision = revision;
    // Open again: not to be deleted after all.
    if (removals_.erase({EngineKey::of(document), document.id.to_string()}) > 0) {
        storeRemovals();
    }
}

void EngineListSync::setEngine(const EngineKey& key, EnginePlayback* playback) {
    if (playback == nullptr) {
        engines_.erase(key);
        return;
    }
    auto& engine = engines_[key];
    if (engine.playback.data() != playback) {
        engine = Engine{};
        engine.playback = playback;
    }
    // Already connected, it will not say so again: what was left pending
    // last time goes now, not at some later reconnect.
    flushRemovals(key);
}

EnginePlayback* EngineListSync::engineFor(const EngineKey& key) const {
    const auto found = engines_.find(key);
    if (found == engines_.end()) {
        return nullptr;
    }
    EnginePlayback* engine = found->second.playback.data();
    return engine != nullptr && engine->active() ? engine : nullptr;
}

EngineListSync::Engine& EngineListSync::stateFor(const EngineKey& key) { return engines_[key]; }

EngineKey EngineListSync::keyOf(const EnginePlayback* playback) const {
    for (const auto& [key, engine] : engines_) {
        if (playback != nullptr && engine.playback.data() == playback) {
            return key;
        }
    }
    return {};
}

void EngineListSync::update(const std::vector<persistence::ListDocument>& documents) {
    std::unordered_set<std::string> present;
    for (const auto& document : documents) {
        // Server URIs of the retired MPD backend: no engine plays those.
        if (document.kind == persistence::ListKind::mpd) {
            continue;
        }
        const auto id = document.id.to_string();
        present.insert(id);
        auto& known = known_[id];
        known.working = document.kind != persistence::ListKind::saved;
        const auto engine = EngineKey::of(document);
        known.engine = engine;
        known.dirty = document.dirty;
        const auto print = fingerprint(document);
        known.local = print;
        if (engineFor(engine) == nullptr) {
            continue;
        }
        // Compared with the engine before anything is sent to it: this
        // window's copy may be older than what changed while it was closed.
        if (!stateFor(engine).compared) {
            continue;
        }
        // A saved list's unsaved edits stay in this window until Save.
        if (!known.working && document.dirty) {
            continue;
        }
        // Saved here after it changed there: the window settles which stands,
        // asked once.
        if (known.conflict) {
            if (!known.asked) {
                known.asked = true;
                emit conflicted(QString::fromStdString(id));
            }
            continue;
        }
        if (known.fingerprint == print) {
            continue;
        }
        if (known.in_flight) {
            known.again = true;
            waiting_.insert_or_assign(id, document);
            continue;
        }
        send(document, print);
    }
    std::vector<EngineKey> keys;
    for (const auto& [key, engine] : engines_) {
        keys.push_back(key);
    }
    for (const auto& key : keys) {
        if (engineFor(key) != nullptr && !stateFor(key).compared) {
            compare(key);
        }
    }
    // Gone from the window: a working list goes from its engine too; a saved
    // one stays there, which is what saving it was for.
    for (auto entry = known_.begin(); entry != known_.end();) {
        if (present.contains(entry->first)) {
            ++entry;
            continue;
        }
        // Whether or not this window knows it was sent: after a reconnect or
        // a failed write it cannot tell, and the engine says if it had none.
        if (entry->second.working) {
            remove(entry->first, entry->second.engine);
        }
        waiting_.erase(entry->first);
        entry = known_.erase(entry);
    }
}

void EngineListSync::reconnected(const EnginePlayback* engine) {
    const auto key = keyOf(engine);
    if (key.isNull()) {
        return;
    }
    auto& state = stateFor(key);
    const QPointer<EnginePlayback> playback = state.playback;
    state = Engine{};
    state.playback = playback;
    flushRemovals(key);
    for (auto& [id, known] : known_) {
        static_cast<void>(id);
        if (known.engine == key) {
            known.fingerprint.reset();
            known.revision.reset();
        }
    }
    emit wantsSave();
}

void EngineListSync::compare(const EngineKey& key) {
    auto& state = stateFor(key);
    auto* engine = engineFor(key);
    if (state.comparing || engine == nullptr) {
        return;
    }
    state.comparing = true;
    ++in_flight_;
    const QPointer self{this};
    engine->request(QStringLiteral("list.all"), Json::object(),
                    [self, key](core::Result<Json> answer) {
                        if (!self) {
                            return;
                        }
                        --self->in_flight_;
                        std::unordered_set<std::string> listed;
                        if (answer) {
                            for (const auto& list : answer->value("lists", Json::array())) {
                                listed.insert(list.value("id", std::string{}));
                            }
                        }
                        // Each list open here that the engine has is fetched
                        // and compared; the rest are the engine's to be given.
                        std::vector<std::string> wanted;
                        for (const auto& [id, known] : self->known_) {
                            if (known.engine == key && listed.contains(id)) {
                                wanted.push_back(id);
                            }
                        }
                        for (const auto& id : wanted) {
                            self->fetch(id, key, true);
                        }
                        self->compared(key);
                    });
}

void EngineListSync::compared(const EngineKey& key) {
    auto& state = stateFor(key);
    if (state.outstanding > 0 || state.compared) {
        return;
    }
    state.comparing = false;
    state.compared = true;
    emit wantsSave();
}

void EngineListSync::fetch(const std::string& id, const EngineKey& key, const bool comparing) {
    auto* engine = engineFor(key);
    if (engine == nullptr) {
        return;
    }
    ++in_flight_;
    if (comparing) {
        ++stateFor(key).outstanding;
    }
    const QPointer self{this};
    engine->request(QStringLiteral("list.get"), Json{{"id", id}},
                    [self, id, key, comparing](core::Result<Json> answer) {
                        if (!self) {
                            return;
                        }
                        --self->in_flight_;
                        const auto settle = [&self, &key, comparing] {
                            if (comparing) {
                                --self->stateFor(key).outstanding;
                                self->compared(key);
                            }
                        };
                        const auto found = self->known_.find(id);
                        if (found == self->known_.end() || !answer) {
                            settle();
                            return;
                        }
                        auto document = document_from(*answer, key);
                        if (!document) {
                            settle();
                            return;
                        }
                        auto& known = found->second;
                        const auto print = fingerprint(*document);
                        const auto revision = answer->value("revision", std::uint64_t{0});
                        if (known.local == print) {
                            // Already what this window shows.
                            known.fingerprint = print;
                            known.revision = revision;
                        } else if (!known.working && known.dirty) {
                            // Changed there while edited here: settled when saved.
                            known.conflict = true;
                        } else {
                            known.fingerprint = print;
                            known.local = print;
                            known.revision = revision;
                            known.conflict = false;
                            known.asked = false;
                            emit self->adopted(*document);
                        }
                        settle();
                    });
}

void EngineListSync::listChanged(const EnginePlayback* engine, const QString& id,
                                 const quint64 revision, const bool deleted) {
    const auto key = keyOf(engine);
    const auto found = known_.find(id.toStdString());
    if (key.isNull() || found == known_.end() || found->second.engine != key) {
        return;
    }
    auto& known = found->second;
    // Deleted elsewhere: that stands, even over a write of this window's on
    // its way -- which, landing after the delete, would make the list again.
    // So it is deleted again, after that write on the same connection.
    if (deleted) {
        const bool writing = known.in_flight;
        waiting_.erase(found->first);
        known_.erase(found);
        if (writing) {
            remove(id.toStdString(), key);
        }
        emit removedElsewhere(id);
        return;
    }
    // This window's own write: its answer brings the revision.
    if (known.in_flight) {
        return;
    }
    if (known.revision && revision <= *known.revision) {
        return;
    }
    fetch(id.toStdString(), key, false);
}

void EngineListSync::keepMine(const QString& id) {
    const auto found = known_.find(id.toStdString());
    if (found == known_.end()) {
        return;
    }
    found->second.conflict = false;
    found->second.asked = false;
    // Written without a revision: whatever changed there, this replaces it.
    found->second.revision.reset();
    found->second.fingerprint.reset();
    emit wantsSave();
}

void EngineListSync::takeTheirs(const QString& id) {
    const auto found = known_.find(id.toStdString());
    if (found == known_.end()) {
        return;
    }
    found->second.conflict = false;
    found->second.asked = false;
    found->second.dirty = false;
    found->second.local.reset();
    fetch(id.toStdString(), found->second.engine, false);
}

void EngineListSync::send(const persistence::ListDocument& document, const std::size_t print) {
    auto* engine = engineFor(EngineKey::of(document));
    if (engine == nullptr) {
        return;
    }
    const auto id = document.id.to_string();
    auto& known = known_[id];
    known.in_flight = true;
    known.fingerprint = print;
    ++in_flight_;
    auto items = Json::array();
    for (const auto& item : document.items) {
        items.push_back(item_json(item));
    }
    Json params{{"id", id},
                {"name", protocol::displayable_text(document.name)},
                {"kind", known.working ? "working" : "saved"},
                {"items", std::move(items)}};
    // A saved list is written against what this window read: someone else's
    // save in between is a conflict, not something to overwrite. A working
    // list is this window's scratch, and the last write stands.
    if (!known.working && known.revision) {
        params["revision"] = *known.revision;
    }
    const QPointer self{this};
    engine->request(QStringLiteral("list.save"), std::move(params),
                    [self, id](core::Result<Json> answer) {
                        if (!self) {
                            return;
                        }
                        --self->in_flight_;
                        const auto found = self->known_.find(id);
                        if (found == self->known_.end()) {
                            return;
                        }
                        auto& entry = found->second;
                        entry.in_flight = false;
                        if (!answer) {
                            if (answer.error().code == core::ErrorCode::conflict) {
                                entry.conflict = true;
                                entry.asked = true;
                                emit self->conflicted(QString::fromStdString(id));
                                return;
                            }
                            // Tried again at the next save -- unless the engine predates
                            // lists and would only refuse again.
                            if (answer.error().code != core::ErrorCode::unsupported) {
                                entry.fingerprint.reset();
                            }
                        } else {
                            entry.revision = answer->value("revision", std::uint64_t{0});
                        }
                        if (!entry.again) {
                            return;
                        }
                        entry.again = false;
                        auto newer = self->waiting_.extract(id);
                        if (!newer.empty()) {
                            const auto& latest = newer.mapped();
                            const auto again = fingerprint(latest);
                            if (entry.fingerprint != again) {
                                self->send(latest, again);
                            }
                        }
                    });
}

void EngineListSync::remove(const std::string& id, const EngineKey& key) {
    // Kept until the engine has answered, across restarts: one that is away
    // is told when it is back, or the list stays on it for good.
    removals_.emplace(key, id);
    storeRemovals();
    flushRemovals(key);
}

void EngineListSync::flushRemovals(const EngineKey& key) {
    auto* engine = engineFor(key);
    if (engine == nullptr || !engine->active()) {
        return;
    }
    for (const auto& pending : removals_) {
        if (pending.first != key || removing_.contains(pending)) {
            continue;
        }
        removing_.insert(pending);
        ++in_flight_;
        const QPointer self{this};
        engine->request(QStringLiteral("list.delete"), Json{{"id", pending.second}},
                        [self, pending](const core::Result<Json>& answer) {
                            if (!self) {
                                return;
                            }
                            --self->in_flight_;
                            self->removing_.erase(pending);
                            // Gone, or an engine too old to have lists: done.
                            // Anything else is tried again on reconnecting.
                            if (answer || answer.error().code == core::ErrorCode::not_found ||
                                answer.error().code == core::ErrorCode::unsupported) {
                                self->removals_.erase(pending);
                                self->storeRemovals();
                            }
                        });
    }
}

void EngineListSync::storeRemovals() const {
    QStringList stored;
    for (const auto& [key, id] : removals_) {
        stored.push_back(key.text() + QLatin1Char('\t') + QString::fromStdString(id));
    }
    QSettings settings;
    if (stored.isEmpty()) {
        settings.remove(QLatin1String(removals_key));
    } else {
        settings.setValue(QLatin1String(removals_key), stored);
    }
}

} // namespace trackknife::bench
