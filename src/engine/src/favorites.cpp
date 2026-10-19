// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/favorites.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/list_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace trackknife::engine {

using protocol::Json;

namespace {

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool same_name(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](const char a, const char b) {
        const auto lower = [](const char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        return lower(a) == lower(b);
    });
}

// The Favorites list: the first saved list of that name.
[[nodiscard]] core::Result<std::optional<persistence::EngineList>>
favorites_list(const Workspace& workspace) {
    auto lists = workspace.load_engine_lists();
    if (!lists) {
        return std::unexpected(std::move(lists.error()));
    }
    for (const auto& summary : *lists) {
        if (summary.kind == persistence::EngineListKind::saved && !summary.draft_of &&
            same_name(summary.name, favorites_name)) {
            return workspace.load_engine_list(summary.id);
        }
    }
    return std::optional<persistence::EngineList>{};
}

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "member", .value = std::move(member)}}};
}

} // namespace

void register_favorite_methods(protocol::Dispatcher& dispatcher, Workspace& workspace,
                               EventSink sink, Player& player, const LocalCatalogue* catalogue) {
    // One toggle at a time: two at once must not both make the list.
    auto lock = std::make_shared<std::mutex>();
    dispatcher.on(
        "favorites.toggle",
        [&workspace, sink, &player, catalogue, lock](const Json& params) -> core::Result<Json> {
            std::string path;
            if (const auto given = params.find("path");
                given != params.end() && !given->is_null()) {
                auto decoded = given->is_string()
                                   ? protocol::decode_raw_path(given->get<std::string>())
                                   : core::Result<std::string>{std::unexpected(core::Error{})};
                if (!decoded || decoded->empty()) {
                    return std::unexpected(bad_params("path is an encoded path", "path"));
                }
                path = std::move(*decoded);
            } else {
                path = player.state().source.raw_path;
                if (path.empty()) {
                    return std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                       .message = "nothing is playing",
                                                       .context = {}});
                }
            }
            const std::scoped_lock held{*lock};
            auto list = favorites_list(workspace);
            if (!list) {
                return std::unexpected(std::move(list.error()));
            }
            std::vector<persistence::EngineListItem> items;
            if (*list) {
                items = (*list)->items;
            }
            const bool was = std::ranges::any_of(
                items, [&path](const auto& item) { return item.raw_path == path; });
            const bool wanted = params.contains("favorite") && params.at("favorite").is_boolean()
                                    ? params.at("favorite").get<bool>()
                                    : !was;
            if (wanted == was) {
                return Json{
                    {"favorite", was},
                    {"list", *list ? Json((*list)->summary.id.to_string()) : Json(nullptr)}};
            }
            if (wanted) {
                // Named as the library names it; a file it does not index, by path.
                persistence::EngineListItem item;
                item.raw_path = path;
                if (catalogue != nullptr) {
                    if (auto described = catalogue->described_tracks({path});
                        described && !described->empty() && described->front()) {
                        item = list_item_of_track(*described->front());
                    }
                }
                items.push_back(std::move(item));
            } else {
                std::erase_if(items, [&path](const auto& item) { return item.raw_path == path; });
            }
            auto saved = workspace.save_engine_list(
                *list ? (*list)->summary.id : core::StableId::random(),
                *list ? (*list)->summary.name : std::string{favorites_name},
                persistence::EngineListKind::saved, items,
                *list ? std::optional{(*list)->summary.revision} : std::nullopt, now_ms());
            if (!saved) {
                return std::unexpected(std::move(saved.error()));
            }
            announce_list_change(sink, &*saved, saved->id);
            return Json{{"favorite", wanted}, {"list", saved->id.to_string()}};
        });

    dispatcher.on("favorites.state", [&workspace](const Json& params) -> core::Result<Json> {
        const auto paths = params.find("paths");
        if (paths == params.end() || !paths->is_array()) {
            return std::unexpected(bad_params("paths is a list of encoded paths", "paths"));
        }
        auto list = favorites_list(workspace);
        if (!list) {
            return std::unexpected(std::move(list.error()));
        }
        auto answers = Json::array();
        for (const auto& value : *paths) {
            const auto decoded = value.is_string()
                                     ? protocol::decode_raw_path(value.get<std::string>())
                                     : core::Result<std::string>{std::unexpected(core::Error{})};
            answers.push_back(decoded && *list &&
                              std::ranges::any_of((*list)->items, [&decoded](const auto& item) {
                                  return item.raw_path == *decoded;
                              }));
        }
        return Json{{"favorites", std::move(answers)},
                    {"list", *list ? Json((*list)->summary.id.to_string()) : Json(nullptr)}};
    });
}

} // namespace trackknife::engine
