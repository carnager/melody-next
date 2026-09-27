// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/file_work_methods.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <string>
#include <utility>

namespace trackknife::engine {

using protocol::Json;

void register_file_work_methods(protocol::Dispatcher& dispatcher) {
    dispatcher.on("metadata.read", [](const Json& params) -> core::Result<Json> {
        const auto paths = params.find("paths");
        if (paths == params.end() || !paths->is_array()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "paths to read are required",
                                               .context = {{.key = "param", .value = "paths"}}});
        }
        if (paths->size() > metadata_read_limit) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "at most " + std::to_string(metadata_read_limit) + " paths per read",
                .context = {{.key = "param", .value = "paths"}}});
        }
        auto files = Json::array();
        for (const auto& encoded : *paths) {
            if (!encoded.is_string()) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "a path is not an encoded path",
                                .context = {{.key = "param", .value = "paths"}}});
            }
            const auto raw_path = protocol::decode_raw_path(encoded.get<std::string>());
            if (!raw_path) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "a path is not an encoded path",
                                .context = {{.key = "param", .value = "paths"}}});
            }
            auto read = metadata::read_local_metadata(*raw_path);
            if (read) {
                Json file = Json::object();
                file["read"] = wire::encode(*read);
                files.push_back(std::move(file));
                continue;
            }
            Json failed = Json::object();
            failed["error"] = wire::encode(read.error());
            failed["revision"] = nullptr;
            if (read.error().code == core::ErrorCode::unsupported) {
                if (auto revision = core::observe_local_source_revision(*raw_path)) {
                    failed["revision"] = wire::encode(*revision);
                }
            }
            files.push_back(std::move(failed));
        }
        return Json{{"files", std::move(files)}};
    });
}

} // namespace trackknife::engine
