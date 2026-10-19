// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/playlist_folder.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/list_methods.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>

namespace trackknife::engine {

using protocol::Json;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view default_folder_name = "Playlists";
constexpr std::string_view deleted_folder_name = ".deleted";
constexpr std::string_view default_extension = ".m3u8";

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::string_view style_name(const lists::PlaylistPathStyle style) {
    switch (style) {
    case lists::PlaylistPathStyle::root_relative:
        return "root";
    case lists::PlaylistPathStyle::playlist_relative:
        return "playlist";
    case lists::PlaylistPathStyle::absolute:
        return "absolute";
    }
    return "root";
}

[[nodiscard]] std::optional<lists::PlaylistPathStyle> style_named(std::string_view name) {
    for (const auto style :
         {lists::PlaylistPathStyle::root_relative, lists::PlaylistPathStyle::playlist_relative,
          lists::PlaylistPathStyle::absolute}) {
        if (style_name(style) == name) {
            return style;
        }
    }
    return std::nullopt;
}

// A list's name as a file's: no slash, no leading dot, not empty.
[[nodiscard]] std::string file_stem(std::string_view name) {
    std::string stem{name};
    std::ranges::replace(stem, '/', '_');
    std::erase(stem, '\0');
    while (!stem.empty() && (stem.front() == '.' || stem.front() == ' ')) {
        stem.erase(stem.begin());
    }
    while (!stem.empty() && stem.back() == ' ') {
        stem.pop_back();
    }
    if (stem.size() > 200U) {
        stem.resize(200U);
    }
    return stem.empty() ? std::string{"Playlist"} : stem;
}

[[nodiscard]] std::string stem_of(const std::string& file_name) {
    return fs::path{file_name}.stem().string();
}

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return std::move(bytes).str();
}

// Whole or not at all: a sibling written, then renamed over it.
[[nodiscard]] bool write_file(const fs::path& path, const std::string& bytes) {
    const auto partial = path.parent_path() / ("." + path.filename().string() + ".partial");
    {
        std::ofstream out{partial, std::ios::binary | std::ios::trunc};
        if (!out) {
            return false;
        }
        out << bytes;
        if (!out.flush()) {
            return false;
        }
    }
    std::error_code error;
    fs::rename(partial, path, error);
    if (error) {
        fs::remove(partial, error);
        return false;
    }
    return true;
}

struct Stat {
    std::int64_t modified_ns{0};
    std::uintmax_t size{0};
};

[[nodiscard]] std::optional<Stat> stat_of(const fs::path& path) {
    std::error_code error;
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto size = fs::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    return Stat{
        .modified_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch())
                .count()),
        .size = size};
}

// What Melody keeps of an item that other players do not read.
[[nodiscard]] std::string melody_of(const persistence::EngineListItem& item) {
    if (!item.segment && !item.source_selection && !item.logical_reference && !item.replay_gain) {
        return {};
    }
    const auto all = list_item_json(item);
    Json kept = Json::object();
    for (const auto* key : {"segment", "selection", "logical", "replay_gain"}) {
        if (const auto found = all.find(key); found != all.end() && !found->is_null()) {
            kept[key] = *found;
        }
    }
    return kept.dump();
}

[[nodiscard]] lists::PlaylistFileEntry entry_of(const persistence::EngineListItem& item) {
    return lists::PlaylistFileEntry{.raw_path = item.raw_path,
                                    .title = item.artist.empty() || item.title.empty()
                                                 ? item.title
                                                 : item.artist + " - " + item.title,
                                    .duration_ms = item.duration_ms,
                                    .melody = melody_of(item)};
}

// The same tracks in the same order, as a file can say them.
[[nodiscard]] bool same_tracks(const std::vector<persistence::EngineListItem>& left,
                               const std::vector<persistence::EngineListItem>& right) {
    return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
        return a.raw_path == b.raw_path && a.segment == b.segment &&
               a.source_selection == b.source_selection &&
               a.logical_reference == b.logical_reference;
    });
}

} // namespace

PlaylistFolder::PlaylistFolder(Workspace& workspace, const LocalCatalogue& catalogue,
                               std::filesystem::path state_file, EventSink sink)
    : workspace_{workspace}, catalogue_{catalogue}, state_file_{std::move(state_file)},
      sink_{std::move(sink)} {
    load_state();
}

PlaylistFolder::~PlaylistFolder() {
    {
        const std::scoped_lock held{wake_mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (poller_.joinable()) {
        poller_.join();
    }
}

EventSink PlaylistFolder::list_sink() {
    return [this](const protocol::Event& event) {
        if (sink_) {
            sink_(event);
        }
        if (event.name != "list.changed") {
            return;
        }
        if (const auto id = core::StableId::parse(event.data.value("id", std::string{}))) {
            changed(*id);
        }
    };
}

void PlaylistFolder::start(const std::chrono::milliseconds every) {
    // On its own thread from the first: a folder on a slow share does not
    // hold up the engine's start.
    poller_ = std::thread{[this, every] {
        while (true) {
            sync();
            {
                std::unique_lock held{wake_mutex_};
                if (wake_.wait_for(held, every, [this] { return stopping_; })) {
                    return;
                }
            }
        }
    }};
}

void PlaylistFolder::sync() {
    const std::scoped_lock held{mutex_};
    sync_locked();
}

std::vector<std::string> PlaylistFolder::music_roots() const {
    std::vector<std::string> roots;
    if (auto found = catalogue_.roots()) {
        // Every one, scanned or not: whether it is there is asked of the
        // disk where it matters.
        for (auto& root : *found) {
            roots.push_back(std::move(root.raw_path));
        }
    }
    return roots;
}

std::optional<std::filesystem::path> PlaylistFolder::folder() const {
    const std::scoped_lock held{mutex_};
    return folder_locked();
}

std::optional<std::filesystem::path> PlaylistFolder::folder_locked() const {
    if (!chosen_folder_.empty()) {
        return fs::path{chosen_folder_};
    }
    const auto roots = music_roots();
    if (roots.empty()) {
        return std::nullopt;
    }
    return fs::path{roots.front()} / default_folder_name;
}

void PlaylistFolder::changed(const core::StableId& id) {
    const std::scoped_lock held{mutex_};
    // What the folder itself said, while it is read, is already its files'.
    if (syncing_) {
        return;
    }
    write_locked(id);
}

void PlaylistFolder::write_locked(const core::StableId& id) {
    const auto folder = folder_locked();
    if (!folder) {
        return;
    }
    const auto id_text = id.to_string();
    auto known = std::ranges::find_if(
        files_, [&id_text](const auto& file) { return file.second.id == id_text; });
    auto list = workspace_.load_engine_list(id);
    if (!list) {
        std::cerr << "melodyd: a playlist was not written: " << list.error().message << "\n";
        return;
    }
    // Gone: its file goes beside, to .deleted, not away.
    if (!*list) {
        if (known != files_.end() && indexed_folder_ == folder->string()) {
            const auto aside = *folder / deleted_folder_name;
            std::error_code error;
            fs::create_directory(aside, error);
            auto target = aside / known->first;
            if (fs::exists(target)) {
                target = aside / (stem_of(known->first) + "." + std::to_string(now_ms()) +
                                  fs::path{known->first}.extension().string());
            }
            fs::rename(*folder / known->first, target, error);
            files_.erase(known);
            save_state_locked();
        }
        return;
    }
    const auto& summary = (*list)->summary;
    if (summary.kind != persistence::EngineListKind::saved || summary.draft_of) {
        return;
    }
    // Playlists in the music folder is made when first needed -- inside a
    // music folder with something in it, so not on the disk beneath a share
    // not mounted. A folder chosen is never made: it is there or waited for.
    std::error_code error;
    if (!fs::is_directory(*folder, error)) {
        const auto parent = folder->parent_path();
        if (!chosen_folder_.empty() || !fs::is_directory(parent, error) ||
            fs::is_empty(parent, error) || error || !fs::create_directory(*folder, error)) {
            return;
        }
    }
    if (indexed_folder_ != folder->string()) {
        files_.clear();
        indexed_folder_ = folder->string();
        known = files_.end();
    }
    // Named as the list is; renamed with it.
    const auto extension = known != files_.end() ? fs::path{known->first}.extension().string()
                                                 : std::string{default_extension};
    auto name = file_stem(summary.name) + extension;
    if (known != files_.end() && known->first != name) {
        if (fs::exists(*folder / name)) {
            name = known->first;
        } else {
            fs::rename(*folder / known->first, *folder / name, error);
            if (error) {
                name = known->first;
            }
        }
    } else if (known == files_.end()) {
        for (int count = 2; fs::exists(*folder / name) && count < 1000; ++count) {
            name = file_stem(summary.name) + " (" + std::to_string(count) + ")" + extension;
        }
    }
    const auto style = known != files_.end() ? known->second.style : style_;
    if (known != files_.end() && known->first != name) {
        files_.erase(known);
    }
    std::vector<lists::PlaylistFileEntry> entries;
    entries.reserve((*list)->items.size());
    std::ranges::transform((*list)->items, std::back_inserter(entries), entry_of);
    const auto path = *folder / name;
    const auto bytes =
        lists::render_playlist_file(entries, path.string(), style, music_roots(), id_text);
    if (read_file(path) != bytes && !write_file(path, bytes)) {
        std::cerr << "melodyd: could not write the playlist " << path.string() << "\n";
        return;
    }
    if (const auto stat = stat_of(path)) {
        files_[name] = Known{
            .id = id_text, .modified_ns = stat->modified_ns, .size = stat->size, .style = style};
    }
    save_state_locked();
}

void PlaylistFolder::sync_locked() {
    const auto folder = folder_locked();
    std::error_code error;
    // A folder not there -- a music folder not mounted -- says nothing of
    // the lists: none is dropped for it.
    if (!folder || !fs::is_directory(*folder, error)) {
        if (folder) {
            // Still the lists are written, once it can be made.
            for (const auto& list : workspace_.load_engine_lists().value_or(
                     std::vector<persistence::EngineListSummary>{})) {
                if (list.kind == persistence::EngineListKind::saved && !list.draft_of) {
                    write_locked(list.id);
                    if (!fs::is_directory(*folder, error)) {
                        break;
                    }
                }
            }
        }
        return;
    }
    if (indexed_folder_ != folder->string()) {
        files_.clear();
        indexed_folder_ = folder->string();
    }
    syncing_ = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } done{syncing_};
    const auto roots = music_roots();
    std::set<std::string> seen;
    std::set<std::string> ids;
    for (const auto& item : fs::directory_iterator{*folder, error}) {
        const auto name = item.path().filename().string();
        if (name.starts_with('.') || !item.is_regular_file(error) ||
            !lists::playlist_format(name)) {
            continue;
        }
        const auto stat = stat_of(item.path());
        if (!stat) {
            continue;
        }
        seen.insert(name);
        const auto known = files_.find(name);
        if (known != files_.end() && known->second.modified_ns == stat->modified_ns &&
            known->second.size == stat->size) {
            ids.insert(known->second.id);
            continue;
        }
        const auto bytes = read_file(item.path());
        if (!bytes) {
            continue;
        }
        auto parsed = lists::parse_playlist_file(
            *bytes, item.path().string(), roots,
            [](const std::string& candidate) { return fs::exists(candidate); });
        if (!parsed) {
            std::cerr << "melodyd: the playlist " << item.path().string()
                      << " was not read: " << parsed.error().message << "\n";
            continue;
        }
        // Which list: as the file says, as it was, or a new one.
        std::optional<core::StableId> id;
        if (const auto said = core::StableId::parse(parsed->list_id);
            said && !ids.contains(said->to_string())) {
            id = *said;
        } else if (known != files_.end() && !ids.contains(known->second.id)) {
            if (const auto kept = core::StableId::parse(known->second.id)) {
                id = *kept;
            }
        }
        if (!id) {
            id = core::StableId::random();
        }
        ids.insert(id->to_string());
        auto list = workspace_.load_engine_list(*id);
        if (!list) {
            continue;
        }
        // The tracks as the library names them; the entries a client knows
        // kept where the same track is.
        std::vector<std::string> paths;
        paths.reserve(parsed->entries.size());
        for (const auto& entry : parsed->entries) {
            paths.push_back(entry.raw_path);
        }
        auto described = catalogue_.described_tracks(paths);
        std::vector<persistence::EngineListItem> items;
        std::vector<bool> taken(*list ? (*list)->items.size() : 0U, false);
        for (std::size_t at = 0; at < parsed->entries.size(); ++at) {
            const auto& entry = parsed->entries[at];
            persistence::EngineListItem made;
            if (described && at < described->size() && (*described)[at]) {
                made = list_item_of_track(*(*described)[at]);
            } else {
                made.raw_path = entry.raw_path;
                made.duration_ms = entry.duration_ms;
                made.title = entry.title;
            }
            if (!entry.melody.empty()) {
                auto melody = Json::parse(entry.melody, nullptr, false);
                if (melody.is_object()) {
                    melody["path"] = protocol::encode_raw_path(entry.raw_path);
                    if (auto read = list_item_from_json(melody)) {
                        made.segment = read->segment;
                        made.source_selection = read->source_selection;
                        made.logical_reference = read->logical_reference;
                        made.replay_gain = read->replay_gain;
                    }
                }
            }
            if (*list) {
                const auto& before = (*list)->items;
                for (std::size_t other = 0; other < before.size(); ++other) {
                    if (!taken[other] && before[other].raw_path == made.raw_path &&
                        before[other].segment == made.segment) {
                        taken[other] = true;
                        // What the engine knew of it, where the library
                        // does not say.
                        if (!(described && at < described->size() && (*described)[at])) {
                            made = before[other];
                        } else {
                            made.entry_id = before[other].entry_id;
                            if (!made.replay_gain) {
                                made.replay_gain = before[other].replay_gain;
                            }
                        }
                        break;
                    }
                }
            }
            items.push_back(std::move(made));
        }
        const auto name_of = stem_of(name);
        const bool fresh = !*list;
        if (fresh || !same_tracks(items, (*list)->items) || (*list)->summary.name != name_of ||
            (*list)->summary.kind != persistence::EngineListKind::saved) {
            auto saved = workspace_.save_engine_list(
                *id, name_of, persistence::EngineListKind::saved, items,
                fresh ? std::optional<std::uint64_t>{} : std::optional{(*list)->summary.revision},
                now_ms());
            if (!saved) {
                std::cerr << "melodyd: the playlist " << item.path().string()
                          << " was not taken in: " << saved.error().message << "\n";
                continue;
            }
            announce_list_change(sink_, &*saved, saved->id);
        }
        files_[name] = Known{.id = id->to_string(),
                             .modified_ns = stat->modified_ns,
                             .size = stat->size,
                             .style = parsed->style};
    }
    if (error) {
        // A folder not read whole drops nothing.
        save_state_locked();
        return;
    }
    // Taken away by another program: its list goes.
    for (auto known = files_.begin(); known != files_.end();) {
        if (seen.contains(known->first)) {
            ++known;
            continue;
        }
        if (const auto id = core::StableId::parse(known->second.id)) {
            if (auto dropped = workspace_.delete_engine_list(*id, std::nullopt);
                dropped && *dropped) {
                announce_list_change(sink_, nullptr, *id);
            }
        }
        known = files_.erase(known);
    }
    // Saved lists with no file yet -- from before the folder, or made while
    // it could not be written -- get theirs.
    std::set<std::string> filed;
    for (const auto& [name, known] : files_) {
        filed.insert(known.id);
    }
    syncing_ = false;
    for (const auto& list :
         workspace_.load_engine_lists().value_or(std::vector<persistence::EngineListSummary>{})) {
        if (list.kind == persistence::EngineListKind::saved && !list.draft_of &&
            !filed.contains(list.id.to_string())) {
            write_locked(list.id);
        }
    }
    save_state_locked();
}

Json PlaylistFolder::describe() const {
    const std::scoped_lock held{mutex_};
    const auto folder = folder_locked();
    const auto roots = music_roots();
    return Json{
        {"folder", folder ? Json(protocol::encode_raw_path(folder->string())) : Json(nullptr)},
        {"default", roots.empty() ? Json(nullptr)
                                  : Json(protocol::encode_raw_path(
                                        (fs::path{roots.front()} / default_folder_name).string()))},
        {"chosen", protocol::encode_raw_path(chosen_folder_)},
        {"style", std::string{style_name(style_)}}};
}

core::Result<Json> PlaylistFolder::configure(const Json& params) {
    {
        const std::scoped_lock held{mutex_};
        if (const auto style = params.find("style"); style != params.end()) {
            const auto named =
                style->is_string() ? style_named(style->get<std::string>()) : std::nullopt;
            if (!named) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "style is root, playlist or absolute",
                                .context = {{.key = "param", .value = "style"}}});
            }
            style_ = *named;
        }
        if (const auto folder = params.find("folder"); folder != params.end()) {
            std::optional<std::string> raw;
            if (folder->is_string()) {
                if (auto decoded = protocol::decode_raw_path(folder->get<std::string>())) {
                    raw = std::move(*decoded);
                }
            }
            std::error_code error;
            if (!raw || (!raw->empty() &&
                         (!raw->starts_with('/') || !fs::is_directory(fs::path{*raw}, error)))) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "folder is a folder on the engine's computer, or empty "
                                           "for Playlists in the first music folder",
                                .context = {{.key = "param", .value = "folder"}}});
            }
            chosen_folder_ = *raw;
        }
        save_state_locked();
        sync_locked();
    }
    return describe();
}

void PlaylistFolder::load_state() {
    const auto bytes = read_file(state_file_);
    if (!bytes) {
        return;
    }
    const auto state = Json::parse(*bytes, nullptr, false);
    if (!state.is_object()) {
        return;
    }
    chosen_folder_ = state.value("folder", std::string{});
    style_ = style_named(state.value("style", std::string{"root"}))
                 .value_or(lists::PlaylistPathStyle::root_relative);
    indexed_folder_ = state.value("indexed_folder", std::string{});
    for (const auto& [name, known] : state.value("files", Json::object()).items()) {
        files_[name] = Known{.id = known.value("id", std::string{}),
                             .modified_ns = known.value("modified_ns", std::int64_t{0}),
                             .size = known.value("size", std::uintmax_t{0}),
                             .style = style_named(known.value("style", std::string{"root"}))
                                          .value_or(lists::PlaylistPathStyle::root_relative)};
    }
}

void PlaylistFolder::save_state_locked() const {
    Json files = Json::object();
    for (const auto& [name, known] : files_) {
        files[name] = Json{{"id", known.id},
                           {"modified_ns", known.modified_ns},
                           {"size", known.size},
                           {"style", std::string{style_name(known.style)}}};
    }
    const Json state{{"folder", chosen_folder_},
                     {"style", std::string{style_name(style_)}},
                     {"indexed_folder", indexed_folder_},
                     {"files", std::move(files)}};
    if (!write_file(state_file_, state.dump(2) + "\n")) {
        std::cerr << "melodyd: could not keep " << state_file_.string() << "\n";
    }
}

void register_playlist_folder_methods(protocol::Dispatcher& dispatcher, PlaylistFolder& folder) {
    dispatcher.on("playlists.folder",
                  [&folder](const Json&) -> core::Result<Json> { return folder.describe(); });
    dispatcher.on("playlists.set_folder", [&folder](const Json& params) -> core::Result<Json> {
        return folder.configure(params);
    });
    dispatcher.on("playlists.sync", [&folder](const Json&) -> core::Result<Json> {
        folder.sync();
        return Json::object();
    });
}

} // namespace trackknife::engine
