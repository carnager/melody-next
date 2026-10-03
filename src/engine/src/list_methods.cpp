// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/list_methods.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/track_description.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "member", .value = std::move(member)}}};
}

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] core::Result<core::StableId> required_id(const Json& params) {
    const auto found = params.find("id");
    if (found == params.end() || !found->is_string()) {
        return std::unexpected(bad_params("a list is named by its id", "id"));
    }
    auto parsed = core::StableId::parse(found->get<std::string>());
    if (!parsed || parsed->is_nil()) {
        return std::unexpected(bad_params("id is not an identity", "id"));
    }
    return *parsed;
}

[[nodiscard]] core::Result<std::optional<std::uint64_t>> optional_revision(const Json& params) {
    const auto found = params.find("revision");
    if (found == params.end() || found->is_null()) {
        return std::optional<std::uint64_t>{};
    }
    if (!found->is_number_unsigned()) {
        return std::unexpected(bad_params("revision must be a whole number", "revision"));
    }
    return std::optional{found->get<std::uint64_t>()};
}

[[nodiscard]] core::Result<std::string> required_name(const Json& params) {
    const auto found = params.find("name");
    if (found == params.end() || !found->is_string()) {
        return std::unexpected(bad_params("a list needs a name", "name"));
    }
    return found->get<std::string>();
}

[[nodiscard]] Json summary_json(const persistence::EngineListSummary& summary) {
    return Json{{"id", summary.id.to_string()},
                {"name", protocol::displayable_text(summary.name)},
                {"kind", summary.kind == persistence::EngineListKind::saved ? "saved" : "working"},
                {"revision", summary.revision},
                {"tracks", summary.tracks},
                {"modified_ms", summary.modified_ms},
                {"draft_of", summary.draft_of ? Json(summary.draft_of->to_string()) : Json(nullptr)},
                {"draft_base", summary.draft_base}};
}

// Text a list item may hold, cut at a character's start; the library keeps
// the whole of it.
[[nodiscard]] std::string item_text(std::string text) {
    constexpr std::size_t most = 4'096U;
    if (text.size() > most) {
        auto end = most;
        while (end > 0U && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        text.resize(end);
    }
    return text;
}

// ADR-0259: a list item for an indexed track, named from the library.
[[nodiscard]] persistence::EngineListItem
item_of_track(persistence::LibraryTrackSnapshot track) {
    const auto first = [&track](const char* field) {
        const auto found = track.facts.fields.find(field);
        return found == track.facts.fields.end() || found->second.empty()
                   ? std::string{}
                   : found->second.front().first;
    };
    return persistence::EngineListItem{
        .entry_id = core::StableId::random(),
        .raw_path = std::move(track.raw_path),
        .logical_reference = std::nullopt,
        .segment = std::nullopt,
        .source_selection = std::nullopt,
        .duration_ms = track.facts.duration_ms >= 0 ? std::optional{track.facts.duration_ms}
                                                    : std::nullopt,
        .title = item_text(std::move(track.facts.title)),
        .artist = item_text(std::move(track.facts.artist)),
        .album = item_text(std::move(track.facts.album)),
        .album_artist = item_text(first("albumartist")),
        .date = item_text(std::move(track.facts.date)),
        .replay_gain = std::nullopt};
}

[[nodiscard]] Json item_json(const persistence::EngineListItem& item) {
    Json rendered{{"entry", item.entry_id.to_string()},
                  {"path", protocol::encode_raw_path(item.raw_path)},
                  {"duration_ms", item.duration_ms ? Json(*item.duration_ms) : Json(nullptr)},
                  {"title", protocol::displayable_text(item.title)},
                  {"artist", protocol::displayable_text(item.artist)},
                  {"album", protocol::displayable_text(item.album)},
                  {"album_artist", protocol::displayable_text(item.album_artist)},
                  {"date", protocol::displayable_text(item.date)}};
    if (item.replay_gain) {
        const auto& gain = *item.replay_gain;
        const auto number = [](const std::optional<double>& value) {
            return value ? Json(*value) : Json(nullptr);
        };
        rendered["replay_gain"] = Json{{"track_gain_db", number(gain.track_gain_db)},
                                       {"track_peak", number(gain.track_peak)},
                                       {"album_gain_db", number(gain.album_gain_db)},
                                       {"album_peak", number(gain.album_peak)}};
    }
    rendered["logical"] = item.logical_reference
                              ? Json(protocol::encode_raw_path(*item.logical_reference))
                              : Json(nullptr);
    rendered["segment"] =
        item.segment ? Json{{"start_sample", item.segment->start_sample},
                            {"end_sample", item.segment->end_sample ? Json(*item.segment->end_sample)
                                                                    : Json(nullptr)}}
                     : Json(nullptr);
    if (item.source_selection) {
        const auto& selection = *item.source_selection;
        rendered["selection"] =
            Json{{"stream_index", selection.audio_stream_index ? Json(*selection.audio_stream_index)
                                                               : Json(nullptr)},
                 {"subsong_index",
                  selection.subsong_index ? Json(*selection.subsong_index) : Json(nullptr)}};
    } else {
        rendered["selection"] = Json(nullptr);
    }
    return rendered;
}

[[nodiscard]] std::optional<int> optional_int(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return found->get<int>();
}

[[nodiscard]] core::Result<persistence::EngineListItem> item_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(bad_params("each item must be an object", "items"));
    }
    const auto path = value.find("path");
    if (path == value.end() || !path->is_string()) {
        return std::unexpected(bad_params("each item needs an encoded path", "items"));
    }
    auto raw_path = protocol::decode_raw_path(path->get<std::string>());
    if (!raw_path) {
        return std::unexpected(bad_params("an item's path is not an encoded path", "items"));
    }
    persistence::EngineListItem item;
    item.raw_path = std::move(*raw_path);
    if (const auto entry = value.find("entry"); entry != value.end() && !entry->is_null()) {
        if (!entry->is_string()) {
            return std::unexpected(bad_params("an item's entry is not an identity", "items"));
        }
        auto parsed = core::StableId::parse(entry->get<std::string>());
        if (!parsed || parsed->is_nil()) {
            return std::unexpected(bad_params("an item's entry is not an identity", "items"));
        }
        item.entry_id = *parsed;
    }
    if (const auto logical = value.find("logical"); logical != value.end() && logical->is_string()) {
        auto decoded = protocol::decode_raw_path(logical->get<std::string>());
        if (!decoded) {
            return std::unexpected(bad_params("an item's logical is not encoded", "items"));
        }
        item.logical_reference = std::move(*decoded);
    }
    if (const auto segment = value.find("segment"); segment != value.end() && segment->is_object()) {
        const auto start = segment->find("start_sample");
        if (start == segment->end() || !start->is_number_integer()) {
            return std::unexpected(bad_params("a segment needs start_sample", "items"));
        }
        item.segment = persistence::ListItemSegment{.start_sample = start->get<std::int64_t>(),
                                                    .end_sample = std::nullopt};
        if (const auto end = segment->find("end_sample");
            end != segment->end() && end->is_number_integer()) {
            item.segment->end_sample = end->get<std::int64_t>();
        }
    }
    if (const auto selection = value.find("selection");
        selection != value.end() && selection->is_object()) {
        item.source_selection = persistence::ListItemSourceSelection{
            .audio_stream_index = optional_int(*selection, "stream_index"),
            .subsong_index = optional_int(*selection, "subsong_index")};
    }
    if (const auto duration = value.find("duration_ms");
        duration != value.end() && duration->is_number_integer() &&
        duration->get<std::int64_t>() >= 0) {
        item.duration_ms = duration->get<std::int64_t>();
    }
    item.title = value.value("title", std::string{});
    item.artist = value.value("artist", std::string{});
    item.album = value.value("album", std::string{});
    item.album_artist = value.value("album_artist", std::string{});
    item.date = value.value("date", std::string{});
    if (const auto gain = value.find("replay_gain"); gain != value.end() && gain->is_object()) {
        persistence::ListItemReplayGain read;
        const auto number = [&gain](const char* member) -> std::optional<double> {
            const auto found = gain->find(member);
            return found != gain->end() && found->is_number() ? std::optional{found->get<double>()}
                                                              : std::nullopt;
        };
        read.track_gain_db = number("track_gain_db");
        read.track_peak = number("track_peak");
        read.album_gain_db = number("album_gain_db");
        read.album_peak = number("album_peak");
        if (read != persistence::ListItemReplayGain{}) {
            item.replay_gain = read;
        }
    }
    return item;
}

// What the player plays for a list entry: the same identity, so a client
// that shows the list can find the entry playing in it.
[[nodiscard]] QueueEntry queue_entry(const persistence::EngineListItem& item) {
    QueueEntry entry;
    entry.entry_id = item.entry_id;
    entry.source.raw_path = item.raw_path;
    if (item.segment) {
        entry.source.segment = formats::SampleRange{.start_sample = item.segment->start_sample,
                                                    .end_sample = item.segment->end_sample};
    }
    if (item.source_selection) {
        entry.source.selection =
            formats::AudioSourceSelection{.stream_index = item.source_selection->audio_stream_index,
                                          .subsong_index = item.source_selection->subsong_index};
    }
    entry.duration_ms = item.duration_ms;
    entry.title = item.title;
    entry.group.album_artist = item.album_artist;
    entry.group.artist = item.artist;
    entry.group.album = item.album;
    entry.group.date = item.date;
    if (item.replay_gain) {
        entry.replay_gain = formats::ReplayGainInfo{.track_gain_db = item.replay_gain->track_gain_db,
                                                    .track_peak = item.replay_gain->track_peak,
                                                    .album_gain_db = item.replay_gain->album_gain_db,
                                                    .album_peak = item.replay_gain->album_peak};
    }
    return entry;
}

[[nodiscard]] core::Result<core::StableId> entry_of(const Json& value) {
    if (!value.is_string()) {
        return std::unexpected(bad_params("an entry is named by its identity", "edits"));
    }
    auto parsed = core::StableId::parse(value.get<std::string>());
    if (!parsed || parsed->is_nil()) {
        return std::unexpected(bad_params("an entry is named by its identity", "edits"));
    }
    return *parsed;
}

// One edit of list.edit, as ADR-0256 has it.
[[nodiscard]] core::Result<persistence::ListEdit> edit_from_json(const Json& value) {
    using persistence::ListEdit;
    if (!value.is_object() || value.size() < 1U) {
        return std::unexpected(bad_params("each edit is an object", "edits"));
    }
    ListEdit edit;
    const auto entries = [&edit](const Json& list) -> core::Result<void> {
        if (!list.is_array()) {
            return std::unexpected(bad_params("an edit names its entries in a list", "edits"));
        }
        for (const auto& each : list) {
            auto entry = entry_of(each);
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
            edit.entries.push_back(*entry);
        }
        return {};
    };
    const auto items = [&edit](const Json& list) -> core::Result<void> {
        if (!list.is_array()) {
            return std::unexpected(bad_params("an edit gives its items in a list", "edits"));
        }
        for (const auto& each : list) {
            auto item = item_from_json(each);
            if (!item) {
                return std::unexpected(std::move(item.error()));
            }
            if (!each.contains("entry")) {
                return std::unexpected(bad_params("an edited item names its entry", "edits"));
            }
            edit.items.push_back(std::move(*item));
        }
        return {};
    };
    core::Result<void> read;
    if (const auto found = value.find("remove"); found != value.end()) {
        edit.kind = ListEdit::Kind::remove;
        read = entries(*found);
    } else if (const auto inserted = value.find("insert"); inserted != value.end()) {
        edit.kind = ListEdit::Kind::insert;
        read = items(*inserted);
    } else if (const auto moved = value.find("move"); moved != value.end()) {
        edit.kind = ListEdit::Kind::move;
        read = entries(*moved);
    } else if (const auto updated = value.find("update"); updated != value.end()) {
        edit.kind = ListEdit::Kind::update;
        read = items(*updated);
    } else {
        return std::unexpected(bad_params("an edit is remove, insert, move or update", "edits"));
    }
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    if (edit.kind == ListEdit::Kind::insert || edit.kind == ListEdit::Kind::move) {
        if (const auto after = value.find("after"); after != value.end() && !after->is_null()) {
            auto anchor = entry_of(*after);
            if (!anchor) {
                return std::unexpected(std::move(anchor.error()));
            }
            edit.after = *anchor;
        }
    }
    return edit;
}

} // namespace

void announce_list_change(const EventSink& sink, const persistence::EngineListSummary* summary,
                          const core::StableId& id) {
    if (!sink) {
        return;
    }
    Json data{{"id", id.to_string()}, {"deleted", summary == nullptr}};
    if (summary != nullptr) {
        data["revision"] = summary->revision;
    }
    sink(protocol::Event{.name = "list.changed", .data = std::move(data)});
}

void register_list_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                           Player& player, const LocalCatalogue* catalogue) {
    // Every client hears of every change, so a list open in two places is
    // refreshed -- or, with unsaved edits, flagged -- in both.
    const auto changed = [sink](const persistence::EngineListSummary* summary,
                                const core::StableId& id) {
        announce_list_change(sink, summary, id);
    };

    dispatcher.on("list.all", [&workspace](const Json&) -> core::Result<Json> {
        auto lists = workspace.load_engine_lists();
        if (!lists) {
            return std::unexpected(std::move(lists.error()));
        }
        auto rendered = Json::array();
        for (const auto& summary : *lists) {
            rendered.push_back(summary_json(summary));
        }
        return Json{{"lists", std::move(rendered)}};
    });

    dispatcher.on("list.get", [&workspace, catalogue](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        auto list = workspace.load_engine_list(*id);
        if (!list) {
            return std::unexpected(std::move(list.error()));
        }
        if (!*list) {
            return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                               .message = "there is no such list",
                                               .context = {{.key = "id", .value = id->to_string()}}});
        }
        auto rendered = summary_json((*list)->summary);
        auto items = Json::array();
        for (const auto& item : (*list)->items) {
            items.push_back(item_json(item));
        }
        if (catalogue != nullptr && params.value("describe", false)) {
            std::vector<std::string> paths;
            paths.reserve((*list)->items.size());
            for (const auto& item : (*list)->items) {
                paths.push_back(item.raw_path);
            }
            auto described = catalogue->described_tracks(paths);
            if (!described) {
                return std::unexpected(std::move(described.error()));
            }
            for (std::size_t index = 0; index < described->size(); ++index) {
                if (const auto& track = (*described)[index]) {
                    items[index]["library"] = describe_track(*track);
                }
            }
        }
        rendered["items"] = std::move(items);
        return rendered;
    });

    dispatcher.on("list.save", [&workspace, changed](const Json& params) -> core::Result<Json> {
        // Without an id it is a new list, and the engine names it.
        core::StableId id = core::StableId::random();
        if (params.contains("id")) {
            auto given = required_id(params);
            if (!given) {
                return std::unexpected(std::move(given.error()));
            }
            id = *given;
        }
        auto name = required_name(params);
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        auto revision = optional_revision(params);
        if (!revision) {
            return std::unexpected(std::move(revision.error()));
        }
        const auto kind_text = params.value("kind", std::string{"working"});
        if (kind_text != "working" && kind_text != "saved") {
            return std::unexpected(bad_params("kind is working or saved", "kind"));
        }
        const auto items = params.find("items");
        if (items == params.end() || !items->is_array()) {
            return std::unexpected(bad_params("items must be a list", "items"));
        }
        std::vector<persistence::EngineListItem> parsed;
        parsed.reserve(items->size());
        for (const auto& value : *items) {
            auto item = item_from_json(value);
            if (!item) {
                return std::unexpected(std::move(item.error()));
            }
            parsed.push_back(std::move(*item));
        }
        auto saved = workspace.save_engine_list(
            id, *name,
            kind_text == "saved" ? persistence::EngineListKind::saved
                                 : persistence::EngineListKind::working,
            parsed, *revision, now_ms());
        if (!saved) {
            return std::unexpected(std::move(saved.error()));
        }
        changed(&*saved, saved->id);
        return summary_json(*saved);
    });

    // ADR-0256: what changed, not the list again. The queue played from the
    // list follows it in the same request.
    dispatcher.on("list.edit",
                  [&workspace, &player, changed](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        const auto revision = params.find("revision");
        if (revision == params.end() || !revision->is_number_unsigned()) {
            return std::unexpected(
                bad_params("an edit is made against the revision it was worked out from",
                           "revision"));
        }
        const auto edits = params.find("edits");
        if (edits == params.end() || !edits->is_array()) {
            return std::unexpected(bad_params("edits must be a list", "edits"));
        }
        std::vector<persistence::ListEdit> parsed;
        parsed.reserve(edits->size());
        std::vector<core::StableId> fresh;
        for (const auto& value : *edits) {
            auto edit = edit_from_json(value);
            if (!edit) {
                return std::unexpected(std::move(edit.error()));
            }
            if (edit->kind == persistence::ListEdit::Kind::insert) {
                for (const auto& item : edit->items) {
                    fresh.push_back(item.entry_id);
                }
            }
            parsed.push_back(std::move(*edit));
        }
        std::optional<std::string> name;
        if (params.contains("name")) {
            auto given = required_name(params);
            if (!given) {
                return std::unexpected(std::move(given.error()));
            }
            name = std::move(*given);
        }
        std::optional<persistence::EngineListKind> kind;
        if (const auto given = params.find("kind"); given != params.end()) {
            if (*given == "working") {
                kind = persistence::EngineListKind::working;
            } else if (*given == "saved") {
                kind = persistence::EngineListKind::saved;
            } else {
                return std::unexpected(bad_params("kind is working or saved", "kind"));
            }
        }
        // The items come back only for a queue to follow: reading a long
        // list back for every batch it is made in would cost its length each
        // time.
        const bool played = player.queue_list() == id->to_string();
        auto edited = workspace.edit_engine_list(
            *id, revision->get<std::uint64_t>(), parsed, now_ms(),
            name ? std::optional<std::string_view>{*name} : std::nullopt, kind, played);
        if (!edited) {
            return std::unexpected(std::move(edited.error()));
        }
        if (played) {
            std::vector<QueueEntry> entries;
            entries.reserve(edited->items.size());
            for (const auto& item : edited->items) {
                entries.push_back(queue_entry(item));
            }
            player.follow_list(id->to_string(), std::move(entries), fresh);
        }
        changed(&edited->summary, *id);
        return summary_json(edited->summary);
    });

    dispatcher.on("list.rename", [&workspace, changed](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        auto name = required_name(params);
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        auto revision = optional_revision(params);
        if (!revision) {
            return std::unexpected(std::move(revision.error()));
        }
        auto renamed = workspace.rename_engine_list(*id, *name, *revision, now_ms());
        if (!renamed) {
            return std::unexpected(std::move(renamed.error()));
        }
        changed(&*renamed, renamed->id);
        return summary_json(*renamed);
    });

    dispatcher.on("list.delete", [&workspace, changed](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        auto revision = optional_revision(params);
        if (!revision) {
            return std::unexpected(std::move(revision.error()));
        }
        // A saved list's draft goes with it, and is told as gone too.
        std::optional<core::StableId> draft;
        if (auto lists = workspace.load_engine_lists()) {
            const auto found =
                std::ranges::find(*lists, std::optional{*id}, &persistence::EngineListSummary::draft_of);
            if (found != lists->end()) {
                draft = found->id;
            }
        }
        auto deleted = workspace.delete_engine_list(*id, *revision);
        if (!deleted) {
            return std::unexpected(std::move(deleted.error()));
        }
        if (*deleted) {
            changed(nullptr, *id);
            if (draft) {
                changed(nullptr, *draft);
            }
        }
        return Json{{"deleted", *deleted}};
    });

    // ADR-0259: a kept search, made here: a working list of a query's
    // matches -- a tkq-1 query, or with `words` a plain word search -- in
    // its order, so they do not travel to a client and back.
    dispatcher.on("list.from_query",
                  [&workspace, catalogue, changed](const Json& params) -> core::Result<Json> {
        if (catalogue == nullptr) {
            return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                               .message = "this engine has no library",
                                               .context = {}});
        }
        const auto source = params.find("query");
        if (source == params.end() || !source->is_string()) {
            return std::unexpected(bad_params("a query is required", "query"));
        }
        auto name = required_name(params);
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        auto compiled = params.value("words", false)
                            ? query::compile_tkq_word_search(source->get<std::string>())
                            : query::compile_tkq(source->get<std::string>());
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        auto paths = catalogue->filter_paths(*compiled);
        if (!paths) {
            return std::unexpected(std::move(paths.error()));
        }
        auto tracks = catalogue->described_tracks(*paths);
        if (!tracks) {
            return std::unexpected(std::move(tracks.error()));
        }
        std::vector<persistence::EngineListItem> items;
        items.reserve(tracks->size());
        for (auto& track : *tracks) {
            if (track) {
                items.push_back(item_of_track(std::move(*track)));
            }
        }
        auto made = workspace.save_engine_list(core::StableId::random(), *name,
                                               persistence::EngineListKind::working, items, 0U,
                                               now_ms());
        if (!made) {
            return std::unexpected(std::move(made.error()));
        }
        changed(&*made, made->id);
        return summary_json(*made);
    });

    // ADR-0259: the draft of a saved list -- the one there is, or a new one.
    dispatcher.on("list.draft", [&workspace, changed](const Json& params) -> core::Result<Json> {
        const auto of = params.find("of");
        auto saved = of != params.end() && of->is_string()
                         ? core::StableId::parse(of->get<std::string>())
                         : core::Result<core::StableId>{std::unexpected(core::Error{})};
        if (!saved) {
            return std::unexpected(bad_params("of names the saved list to draft", "of"));
        }
        auto draft = workspace.draft_engine_list(*saved, core::StableId::random(), now_ms());
        if (!draft) {
            return std::unexpected(std::move(draft.error()));
        }
        changed(&*draft, draft->id);
        return summary_json(*draft);
    });

    // ADR-0259: a draft saved into the list it drafts, and gone. A queue
    // played from the draft is played from the list from then on.
    dispatcher.on("list.commit",
                  [&workspace, &player, changed](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        auto saved = workspace.commit_engine_list_draft(*id, params.value("force", false),
                                                        now_ms());
        if (!saved) {
            return std::unexpected(std::move(saved.error()));
        }
        static_cast<void>(player.adopt_queue_list(id->to_string(), saved->id.to_string()));
        changed(nullptr, *id);
        changed(&*saved, saved->id);
        return summary_json(*saved);
    });

    // Files moved or renamed -- by Trackknife, which does the file work --
    // followed in every list here: {"moves": [{"from", "to"}]}, encoded paths.
    dispatcher.on("list.relocate", [&workspace, changed](const Json& params) -> core::Result<Json> {
        const auto moves = params.find("moves");
        if (moves == params.end() || !moves->is_array()) {
            return std::unexpected(bad_params("moves must be a list", "moves"));
        }
        std::vector<std::pair<std::string, std::string>> decoded;
        decoded.reserve(moves->size());
        for (const auto& move : *moves) {
            auto from = protocol::decode_raw_path(move.value("from", std::string{}));
            auto to = protocol::decode_raw_path(move.value("to", std::string{}));
            if (!from || !to) {
                return std::unexpected(bad_params("each move needs encoded from and to", "moves"));
            }
            decoded.emplace_back(std::move(*from), std::move(*to));
        }
        auto relocated = workspace.relocate_engine_list_paths(decoded, now_ms());
        if (!relocated) {
            return std::unexpected(std::move(relocated.error()));
        }
        auto ids = Json::array();
        for (const auto& summary : *relocated) {
            changed(&summary, summary.id);
            ids.push_back(summary.id.to_string());
        }
        return Json{{"changed", std::move(ids)}};
    });

    // For the phone and the CLI, which hold no list of their own: the list
    // becomes the queue and plays, from its first entry or the one named.
    dispatcher.on("list.play", [&workspace, &player](const Json& params) -> core::Result<Json> {
        auto id = required_id(params);
        if (!id) {
            return std::unexpected(std::move(id.error()));
        }
        auto list = workspace.load_engine_list(*id);
        if (!list) {
            return std::unexpected(std::move(list.error()));
        }
        if (!*list || (*list)->items.empty()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::not_found,
                .message = *list ? "the list is empty" : "there is no such list",
                .context = {{.key = "id", .value = id->to_string()}}});
        }
        auto start = (*list)->items.front().entry_id;
        if (const auto entry = params.find("entry"); entry != params.end() && entry->is_string()) {
            auto parsed = core::StableId::parse(entry->get<std::string>());
            if (!parsed) {
                return std::unexpected(bad_params("entry is not an identity", "entry"));
            }
            start = *parsed;
        }
        std::vector<QueueEntry> queue;
        queue.reserve((*list)->items.size());
        for (const auto& item : (*list)->items) {
            queue.push_back(queue_entry(item));
        }
        // ADR-0253: the queue knows its list, for the list's continuation.
        player.replace_queue(std::move(queue), id->to_string());
        if (auto played = player.play_entry(start); !played) {
            return std::unexpected(std::move(played.error()));
        }
        return Json{{"playing", start.to_string()}};
    });
}

} // namespace trackknife::engine
