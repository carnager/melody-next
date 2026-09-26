// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/list_methods.hpp"

#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <utility>

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
                {"modified_ms", summary.modified_ms}};
}

[[nodiscard]] Json item_json(const persistence::EngineListItem& item) {
    Json rendered{{"entry", item.entry_id.to_string()},
                  {"path", protocol::encode_raw_path(item.raw_path)},
                  {"duration_ms", item.duration_ms ? Json(*item.duration_ms) : Json(nullptr)},
                  {"title", protocol::displayable_text(item.title)},
                  {"artist", protocol::displayable_text(item.artist)},
                  {"album", protocol::displayable_text(item.album)}};
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
    entry.group.artist = item.artist;
    entry.group.album = item.album;
    return entry;
}

} // namespace

void register_list_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                           Player& player) {
    // Every client hears of every change, so a list open in two places is
    // refreshed -- or, with unsaved edits, flagged -- in both.
    const auto changed = [sink](const persistence::EngineListSummary* summary,
                                const core::StableId& id) {
        if (!sink) {
            return;
        }
        Json data{{"id", id.to_string()}, {"deleted", summary == nullptr}};
        if (summary != nullptr) {
            data["revision"] = summary->revision;
        }
        sink(protocol::Event{.name = "list.changed", .data = std::move(data)});
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

    dispatcher.on("list.get", [&workspace](const Json& params) -> core::Result<Json> {
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
        auto deleted = workspace.delete_engine_list(*id, *revision);
        if (!deleted) {
            return std::unexpected(std::move(deleted.error()));
        }
        if (*deleted) {
            changed(nullptr, *id);
        }
        return Json{{"deleted", *deleted}};
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
        player.replace_queue(std::move(queue));
        if (auto played = player.play_entry(start); !played) {
            return std::unexpected(std::move(played.error()));
        }
        return Json{{"playing", start.to_string()}};
    });
}

} // namespace trackknife::engine
