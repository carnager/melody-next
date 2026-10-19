// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/naming_methods.hpp"

#include "trackknife/core/environment.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/workspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

[[nodiscard]] core::Result<Json> layouts(const Workspace& workspace) {
    auto saved = workspace.load_output_layout_profiles();
    if (!saved) {
        return std::unexpected(std::move(saved.error()));
    }
    auto list = Json::array();
    for (const auto& layout : *saved) {
        list.push_back(wire::encode(layout));
    }
    return Json{{"layouts", std::move(list)}};
}

[[nodiscard]] core::Result<Json> destinations(const Workspace& workspace) {
    auto saved = workspace.load_destination_profiles();
    if (!saved) {
        return std::unexpected(std::move(saved.error()));
    }
    auto list = Json::array();
    for (const auto& destination : *saved) {
        list.push_back(wire::encode(destination));
    }
    return Json{{"destinations", std::move(list)}};
}

} // namespace

void register_naming_methods(protocol::Dispatcher& dispatcher, Workspace& workspace, EventSink sink,
                             const LocalCatalogue* catalogue) {
    dispatcher.on("layouts.list",
                  [&workspace](const Json&) -> core::Result<Json> { return layouts(workspace); });

    // Only ever adds, updates, or removes what it is told to: a client with
    // an older or smaller set -- another Trackknife, a copy sent late -- must
    // not take layouts away (this computer's engine shares Trackknife's
    // workspace, so what it holds is Trackknife's own).
    dispatcher.on("layouts.put", [&workspace](const Json& params) -> core::Result<Json> {
        const auto given = params.find("layouts");
        if (given == params.end() || !given->is_array()) {
            return std::unexpected(bad_params("the layouts are required", "layouts"));
        }
        std::vector<persistence::SavedOutputLayoutProfile> wanted;
        for (const auto& value : *given) {
            auto layout = wire::decode_saved_layout(value);
            if (!layout) {
                return std::unexpected(std::move(layout.error()));
            }
            wanted.push_back(std::move(*layout));
        }
        std::vector<core::StableId> removed;
        if (const auto listed = params.find("removed"); listed != params.end()) {
            if (!listed->is_array()) {
                return std::unexpected(bad_params("removed is a list of ids", "removed"));
            }
            for (const auto& value : *listed) {
                auto id = value.is_string()
                              ? core::StableId::parse(value.get<std::string>())
                              : core::Result<core::StableId>{std::unexpected(core::Error{})};
                if (!id) {
                    return std::unexpected(bad_params("removed is a list of ids", "removed"));
                }
                removed.push_back(*id);
            }
        }
        for (const auto& id : removed) {
            if (auto gone = workspace.remove_output_layout_profile(id); !gone) {
                return std::unexpected(std::move(gone.error()));
            }
        }
        auto held = workspace.load_output_layout_profiles();
        if (!held) {
            return std::unexpected(std::move(held.error()));
        }
        for (const auto& layout : wanted) {
            if (std::ranges::contains(*held, layout)) {
                continue;
            }
            // A name is one layout's: another here by the same name is the
            // same layout under an older identity, and gives way.
            for (const auto& other : *held) {
                if (other.id != layout.id && other.profile.name == layout.profile.name) {
                    if (auto gone = workspace.remove_output_layout_profile(other.id); !gone) {
                        return std::unexpected(std::move(gone.error()));
                    }
                }
            }
            if (auto saved = workspace.upsert_output_layout_profile(layout); !saved) {
                return std::unexpected(std::move(saved.error()));
            }
        }
        return layouts(workspace);
    });

    dispatcher.on("destinations.list", [&workspace](const Json&) -> core::Result<Json> {
        return destinations(workspace);
    });

    const auto changed = [sink] {
        if (sink) {
            sink(protocol::Event{.name = "destinations.changed", .data = Json::object()});
        }
    };

    dispatcher.on(
        "destinations.save", [&workspace, changed](const Json& params) -> core::Result<Json> {
            const auto given = params.find("destination");
            if (given == params.end()) {
                return std::unexpected(bad_params("a destination is required", "destination"));
            }
            auto destination = wire::decode_saved_destination(*given);
            if (!destination) {
                return std::unexpected(std::move(destination.error()));
            }
            if (auto saved = workspace.upsert_destination_profile(*destination); !saved) {
                return std::unexpected(std::move(saved.error()));
            }
            changed();
            return destinations(workspace);
        });

    dispatcher.on("destinations.remove",
                  [&workspace, changed](const Json& params) -> core::Result<Json> {
                      const auto given = params.find("id");
                      if (given == params.end() || !given->is_string()) {
                          return std::unexpected(bad_params("an id is required", "id"));
                      }
                      auto id = core::StableId::parse(given->get<std::string>());
                      if (!id) {
                          return std::unexpected(bad_params("an id is required", "id"));
                      }
                      if (auto removed = workspace.remove_destination_profile(*id); !removed) {
                          return std::unexpected(std::move(removed.error()));
                      }
                      changed();
                      return destinations(workspace);
                  });

    dispatcher.on("folders.list", [catalogue](const Json& params) -> core::Result<Json> {
        std::filesystem::path folder;
        if (const auto given = params.find("path"); given != params.end() && !given->is_null()) {
            auto decoded = given->is_string()
                               ? protocol::decode_raw_path(given->get<std::string>())
                               : core::Result<std::string>{std::unexpected(core::Error{})};
            if (!decoded || decoded->empty() || decoded->front() != '/') {
                return std::unexpected(bad_params("path is an absolute encoded path", "path"));
            }
            folder = std::filesystem::path{*decoded}.lexically_normal();
        } else {
            const auto* home = trackknife::core::environment_variable("HOME");
            folder = home != nullptr && *home == '/' ? std::filesystem::path{home}
                                                     : std::filesystem::path{"/"};
        }
        if (folder.native().size() > 1U && folder.native().back() == '/') {
            folder = folder.parent_path();
        }
        std::error_code error;
        std::filesystem::directory_iterator entries{
            folder, std::filesystem::directory_options::skip_permission_denied, error};
        if (error) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::not_found,
                            .message = "the folder cannot be read: " + error.message(),
                            .context = {{.key = "path", .value = folder.native()}}});
        }
        const bool links = params.value("links", false);
        std::vector<std::string> names;
        std::vector<std::string> linked;
        for (; entries != std::filesystem::directory_iterator{}; entries.increment(error)) {
            if (error) {
                break;
            }
            std::error_code kind;
            // What a move may go below: real folders, not links to them --
            // unless links are asked for, and said.
            const auto type = entries->symlink_status(kind).type();
            if (type == std::filesystem::file_type::directory) {
                names.push_back(entries->path().filename().native());
            } else if (links && type == std::filesystem::file_type::symlink &&
                       entries->is_directory(kind)) {
                names.push_back(entries->path().filename().native());
                linked.push_back(entries->path().filename().native());
            }
            if (names.size() > folder_list_limit) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::limit_exceeded,
                                .message = "the folder holds more folders than can be listed",
                                .context = {{.key = "path", .value = folder.native()}}});
            }
        }
        std::ranges::sort(names);
        auto listed = Json::array();
        for (const auto& name : names) {
            listed.push_back(protocol::encode_raw_path(name));
        }
        auto links_listed = Json::array();
        for (const auto& name : linked) {
            links_listed.push_back(protocol::encode_raw_path(name));
        }
        // Where browsing starts: home, and the library's music folders.
        const auto* home = trackknife::core::environment_variable("HOME");
        auto roots = Json::array();
        if (catalogue != nullptr) {
            if (auto found = catalogue->roots()) {
                for (const auto& root : *found) {
                    roots.push_back(protocol::encode_raw_path(root.raw_path));
                }
            }
        }
        const auto parent = folder.parent_path();
        return Json{{"path", protocol::encode_raw_path(folder.native())},
                    {"parent",
                     parent == folder ? Json() : Json(protocol::encode_raw_path(parent.native()))},
                    {"folders", std::move(listed)},
                    {"linked", std::move(links_listed)},
                    {"home", home != nullptr && *home == '/' ? Json(protocol::encode_raw_path(home))
                                                             : Json(nullptr)},
                    {"roots", std::move(roots)}};
    });

    dispatcher.on("folders.make", [](const Json& params) -> core::Result<Json> {
        const auto given = [&params](const char* key) -> std::optional<std::string> {
            const auto found = params.find(key);
            if (found == params.end() || !found->is_string()) {
                return std::nullopt;
            }
            auto decoded = protocol::decode_raw_path(found->get<std::string>());
            return decoded ? std::optional{std::move(*decoded)} : std::nullopt;
        };
        const auto path = given("path");
        if (!path || !path->starts_with('/')) {
            return std::unexpected(bad_params("path is an absolute encoded path", "path"));
        }
        const auto name = params.value("name", std::string{});
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of(std::string_view{"/\0", 2}) != std::string::npos) {
            return std::unexpected(bad_params("name is a folder's name, without a slash", "name"));
        }
        const auto made = std::filesystem::path{*path} / name;
        std::error_code error;
        if (std::filesystem::exists(made, error)) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::conflict,
                            .message = "there is already something of that name",
                            .context = {{.key = "path", .value = made.native()}}});
        }
        if (!std::filesystem::create_directory(made, error) || error) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::io,
                            .message = "the folder could not be made: " + error.message(),
                            .context = {{.key = "path", .value = made.native()}}});
        }
        return Json{{"path", protocol::encode_raw_path(made.native())}};
    });
}

} // namespace trackknife::engine
