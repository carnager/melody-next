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

} // namespace

EngineListSync::EngineListSync(QObject* parent) : QObject(parent) {}

void EngineListSync::setEngines(EnginePlayback* local, EnginePlayback* remote) {
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
        // A saved list's unsaved edits stay in this window until Save.
        if (!known.working && document.dirty) {
            continue;
        }
        const auto print = fingerprint(document);
        if (known.fingerprint == print || engineFor(document.remote) == nullptr) {
            continue;
        }
        if (known.in_flight) {
            known.again = true;
            waiting_.insert_or_assign(id, document);
            continue;
        }
        send(document, print);
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

void EngineListSync::forget(const EnginePlayback* engine) {
    for (auto& [id, known] : known_) {
        static_cast<void>(id);
        const EnginePlayback* owner = known.remote ? remote_.data() : local_.data();
        if (owner == engine) {
            known.fingerprint.reset();
            known.revision.reset();
        }
    }
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
                            // Tried again at the next save -- unless the engine
                            // predates lists and would only refuse again.
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
