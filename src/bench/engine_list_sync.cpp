// SPDX-License-Identifier: GPL-3.0-only

#include "bench/engine_list_sync.hpp"

#include "bench/engine_playback.hpp"
#include "trackknife/protocol/message.hpp"

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
                                                                     const bool remote) {
    auto id = core::StableId::parse(answer.value("id", std::string{}));
    if (!id) {
        return std::nullopt;
    }
    persistence::ListDocument document{
        .id = *id,
        .kind = answer.value("kind", std::string{}) == "saved" ? persistence::ListKind::saved
                                                               : persistence::ListKind::scratch,
        .name = answer.value("name", std::string{}),
        .pinned = false,
        .dirty = false,
        .items = {},
        .remote = remote};
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

EngineListSync::EngineListSync(QObject* parent) : QObject(parent) {}

std::optional<persistence::ListDocument>
EngineListSync::documentFromAnswer(const protocol::Json& answer, const bool remote) {
    return document_from(answer, remote);
}

void EngineListSync::opened(const persistence::ListDocument& document, const std::uint64_t revision) {
    auto& known = known_[document.id.to_string()];
    known.working = document.kind != persistence::ListKind::saved;
    known.remote = document.remote;
    known.dirty = false;
    known.fingerprint = fingerprint(document);
    known.local = known.fingerprint;
    known.revision = revision;
}

void EngineListSync::setEngines(EnginePlayback* local, EnginePlayback* remote) {
    if (remote_.data() != remote) {
        remote_state_ = Engine{};
    }
    if (local_.data() != local) {
        local_state_ = Engine{};
    }
    local_ = local;
    remote_ = remote;
}

EnginePlayback* EngineListSync::engineFor(const bool remote) const {
    EnginePlayback* engine = remote ? remote_.data() : local_.data();
    return engine != nullptr && engine->active() ? engine : nullptr;
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
        known.remote = document.remote;
        known.dirty = document.dirty;
        const auto print = fingerprint(document);
        known.local = print;
        if (engineFor(document.remote) == nullptr) {
            continue;
        }
        // Compared with the engine before anything is sent to it: this
        // window's copy may be older than what changed while it was closed.
        if (!stateFor(document.remote).compared) {
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
    for (const bool remote : {false, true}) {
        if (engineFor(remote) != nullptr && !stateFor(remote).compared) {
            compare(remote);
        }
    }
    // Gone from the window: a working list goes from its engine too; a saved
    // one stays there, which is what saving it was for.
    for (auto entry = known_.begin(); entry != known_.end();) {
        if (present.contains(entry->first)) {
            ++entry;
            continue;
        }
        if (entry->second.working && entry->second.fingerprint) {
            remove(entry->first, entry->second.remote);
        }
        waiting_.erase(entry->first);
        entry = known_.erase(entry);
    }
}

void EngineListSync::reconnected(const EnginePlayback* engine) {
    const bool remote = engine != nullptr && engine == remote_.data();
    stateFor(remote) = Engine{};
    for (auto& [id, known] : known_) {
        static_cast<void>(id);
        if (known.remote == remote) {
            known.fingerprint.reset();
            known.revision.reset();
        }
    }
    emit wantsSave();
}

void EngineListSync::compare(const bool remote) {
    auto& state = stateFor(remote);
    auto* engine = engineFor(remote);
    if (state.comparing || engine == nullptr) {
        return;
    }
    state.comparing = true;
    ++in_flight_;
    const QPointer self{this};
    engine->request(QStringLiteral("list.all"), Json::object(),
                    [self, remote](core::Result<Json> answer) {
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
                            if (known.remote == remote && listed.contains(id)) {
                                wanted.push_back(id);
                            }
                        }
                        for (const auto& id : wanted) {
                            self->fetch(id, remote, true);
                        }
                        self->compared(remote);
                    });
}

void EngineListSync::compared(const bool remote) {
    auto& state = stateFor(remote);
    if (state.outstanding > 0 || state.compared) {
        return;
    }
    state.comparing = false;
    state.compared = true;
    emit wantsSave();
}

void EngineListSync::fetch(const std::string& id, const bool remote, const bool comparing) {
    auto* engine = engineFor(remote);
    if (engine == nullptr) {
        return;
    }
    ++in_flight_;
    if (comparing) {
        ++stateFor(remote).outstanding;
    }
    const QPointer self{this};
    engine->request(
        QStringLiteral("list.get"), Json{{"id", id}},
        [self, id, remote, comparing](core::Result<Json> answer) {
            if (!self) {
                return;
            }
            --self->in_flight_;
            const auto settle = [&self, remote, comparing] {
                if (comparing) {
                    --self->stateFor(remote).outstanding;
                    self->compared(remote);
                }
            };
            const auto found = self->known_.find(id);
            if (found == self->known_.end() || !answer) {
                settle();
                return;
            }
            auto document = document_from(*answer, remote);
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
    const bool remote = engine != nullptr && engine == remote_.data();
    const auto found = known_.find(id.toStdString());
    if (found == known_.end() || found->second.remote != remote) {
        return;
    }
    auto& known = found->second;
    // This window's own write: its answer brings the revision.
    if (known.in_flight) {
        return;
    }
    if (deleted) {
        waiting_.erase(found->first);
        known_.erase(found);
        emit removedElsewhere(id);
        return;
    }
    if (known.revision && revision <= *known.revision) {
        return;
    }
    fetch(id.toStdString(), remote, false);
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
    fetch(id.toStdString(), found->second.remote, false);
}

void EngineListSync::send(const persistence::ListDocument& document, const std::size_t print) {
    auto* engine = engineFor(document.remote);
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
    engine->request(
        QStringLiteral("list.save"), std::move(params), [self, id](core::Result<Json> answer) {
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

void EngineListSync::remove(const std::string& id, const bool remote) {
    auto* engine = engineFor(remote);
    if (engine == nullptr) {
        return;
    }
    ++in_flight_;
    const QPointer self{this};
    engine->request(QStringLiteral("list.delete"), Json{{"id", id}},
                    [self](const core::Result<Json>&) {
                        if (self) {
                            --self->in_flight_;
                        }
                    });
}

} // namespace trackknife::bench
