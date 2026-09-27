// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/file_work_methods.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <ctime>

#include <string>
#include <utility>

namespace trackknife::engine {

using protocol::Json;

FileWorkRecovery recover_file_work(const std::filesystem::path& database,
                                   LocalCatalogue& catalogue) {
    FileWorkRecovery recovery;
    auto opened = persistence::SqliteMetadataOperationJournal::open(database);
    if (!opened) {
        recovery.error = std::move(opened.error());
        return recovery;
    }
    auto journal = std::move(*opened);
    const operations::MetadataDependentStateCommitter dependent =
        [&catalogue](const operations::MetadataCommitResult& result) -> core::Result<void> {
        auto refreshed = catalogue.refresh({result.source_raw_path});
        return refreshed ? core::Result<void>{} : std::unexpected(std::move(refreshed.error()));
    };
    auto recovered = operations::recover_metadata_operations(journal, dependent);
    if (!recovered) {
        recovery.error = std::move(recovered.error());
        return recovery;
    }
    for (const auto& result : *recovered) {
        if (result.outcome != operations::MetadataRecoveryOutcome::needs_reconciliation) {
            ++recovery.recovered;
        }
    }
    // No undo is offered, so a finished write keeps no backup (as in
    // Trackknife, whose policy this is).
    constexpr operations::MetadataBackupRetentionPolicy release_all{
        .maximum_age_seconds = 0, .maximum_entries = 0U, .maximum_total_bytes = 0U};
    if (auto maintained = operations::maintain_metadata_backups(
            journal, release_all, static_cast<std::int64_t>(std::time(nullptr)));
        !maintained) {
        recovery.error = std::move(maintained.error());
    }
    return recovery;
}

void register_file_work_methods(protocol::Dispatcher& dispatcher, std::filesystem::path database,
                                FileWorkRecovery recovery) {
    dispatcher.on("media.probe", [](const Json& params) -> core::Result<Json> {
        const auto paths = params.find("paths");
        if (paths == params.end() || !paths->is_array() || paths->size() > metadata_read_limit) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "up to " + std::to_string(metadata_read_limit) + " paths are required",
                .context = {{.key = "param", .value = "paths"}}});
        }
        auto files = Json::array();
        for (const auto& encoded : *paths) {
            const auto raw_path = encoded.is_string()
                                      ? protocol::decode_raw_path(encoded.get<std::string>())
                                      : core::Result<std::string>{std::unexpected(core::Error{})};
            if (!raw_path) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "a path is not an encoded path",
                                .context = {{.key = "param", .value = "paths"}}});
            }
            Json file = Json::object();
            if (auto facts = probe_local_technicals(*raw_path, {})) {
                file["technicals"] =
                    Json{{"codec", facts->codec},       {"sample_rate", facts->sample_rate},
                         {"bits", facts->bits},         {"channels", facts->channels},
                         {"bit_rate", facts->bit_rate}, {"duration_ms", facts->duration_ms}};
            } else {
                file["error"] = wire::encode(facts.error());
            }
            files.push_back(std::move(file));
        }
        return Json{{"files", std::move(files)}};
    });

    dispatcher.on(
        "metadata.interrupted",
        [database = std::move(database),
         recovery = std::move(recovery)](const Json&) -> core::Result<Json> {
            auto interrupted = Json::array();
            std::optional<core::Error> error = recovery.error;
            if (!database.empty()) {
                auto opened = persistence::SqliteMetadataOperationJournal::open(database);
                auto incomplete =
                    opened ? opened->load_incomplete()
                           : core::Result<std::vector<operations::MetadataOperationJournalRecord>>{
                                 std::unexpected(opened.error())};
                if (!incomplete) {
                    error = std::move(incomplete.error());
                } else {
                    for (const auto& record : *incomplete) {
                        if (record.state !=
                            operations::MetadataOperationJournalState::needs_reconciliation) {
                            continue;
                        }
                        interrupted.push_back(
                            Json{{"id", record.id.to_string()},
                                 {"path", protocol::encode_raw_path(record.source_raw_path)},
                                 {"message", record.failure ? Json(protocol::displayable_text(
                                                                  record.failure->message))
                                                            : Json()}});
                    }
                }
            }
            return Json{{"recovered", recovery.recovered},
                        {"error", error ? wire::encode(*error) : Json()},
                        {"interrupted", std::move(interrupted)}};
        });

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
