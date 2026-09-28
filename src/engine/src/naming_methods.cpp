// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/naming_methods.hpp"

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

void register_naming_methods(protocol::Dispatcher& dispatcher, Workspace& workspace,
                             EventSink sink) {
    dispatcher.on("layouts.list",
                  [&workspace](const Json&) -> core::Result<Json> { return layouts(workspace); });

    dispatcher.on("layouts.set", [&workspace](const Json& params) -> core::Result<Json> {
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
        auto held = workspace.load_output_layout_profiles();
        if (!held) {
            return std::unexpected(std::move(held.error()));
        }
        // Gone ones first: a name moves from one layout to another freely.
        for (const auto& layout : *held) {
            if (std::ranges::none_of(wanted, [&layout](const auto& kept) {
                    return kept.id == layout.id && kept == layout;
                })) {
                if (auto removed = workspace.remove_output_layout_profile(layout.id); !removed) {
                    return std::unexpected(std::move(removed.error()));
                }
            }
        }
        for (const auto& layout : wanted) {
            if (std::ranges::contains(*held, layout)) {
                continue;
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

    dispatcher.on("folders.list", [](const Json& params) -> core::Result<Json> {
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
            const auto* home = std::getenv("HOME");
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
        std::vector<std::string> names;
        for (; entries != std::filesystem::directory_iterator{}; entries.increment(error)) {
            if (error) {
                break;
            }
            std::error_code kind;
            // What a move may go below: real folders, not links to them.
            if (entries->symlink_status(kind).type() == std::filesystem::file_type::directory) {
                names.push_back(entries->path().filename().native());
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
        const auto parent = folder.parent_path();
        return Json{{"path", protocol::encode_raw_path(folder.native())},
                    {"parent",
                     parent == folder ? Json() : Json(protocol::encode_raw_path(parent.native()))},
                    {"folders", std::move(listed)}};
    });
}

} // namespace trackknife::engine
