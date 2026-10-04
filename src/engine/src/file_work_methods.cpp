// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/file_work_methods.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/list_methods.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/undo_copies.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>

#include <string>
#include <thread>
#include <utility>

namespace trackknife::engine {

using protocol::Json;

MoveFollower follow_moves(Workspace& workspace, LocalCatalogue& catalogue, Player* player,
                          EventSink sink) {
    return [&workspace, &catalogue, player, sink = std::move(sink)](
               const operations::FilePublicationCommitResult& result,
               const metadata::MetadataDocument* published) -> core::Result<void> {
        auto relocated = workspace.relocate_local_source(persistence::LocalSourceRelocation{
            .operation_id = result.journal_id,
            .source_reference = result.source_raw_path,
            .target_reference = result.target_raw_path,
            .previous_revision = result.source_revision,
            .published_revision = result.target_revision,
            .published_document = published == nullptr ? std::nullopt : std::optional{*published},
        });
        if (!relocated) {
            return std::unexpected(std::move(relocated.error()));
        }
        if (auto refreshed = catalogue.refresh({result.source_raw_path, result.target_raw_path});
            !refreshed) {
            return std::unexpected(std::move(refreshed.error()));
        }
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        auto lists = workspace.relocate_engine_list_paths(
            {{result.source_raw_path, result.target_raw_path}}, now_ms);
        if (!lists) {
            return std::unexpected(std::move(lists.error()));
        }
        for (const auto& summary : *lists) {
            announce_list_change(sink, &summary, summary.id);
        }
        if (player != nullptr) {
            player->relocate(result.source_raw_path, result.target_raw_path);
        }
        return {};
    };
}

namespace {

// The tags of a file a move published with new content, read back where it
// now is -- only while it is still what was published.
[[nodiscard]] core::Result<std::optional<metadata::MetadataDocument>>
published_document(const operations::FilePublicationCommitResult& result) {
    if (result.content != operations::FilePublicationContentKind::prepared_destination_artifact) {
        return std::nullopt;
    }
    auto read = metadata::read_local_metadata(result.target_raw_path);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    if (read->source_revision != result.target_revision) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Published destination artifact changed before metadata reconciliation",
            .context = {},
        });
    }
    return std::move(read->document);
}

} // namespace

FileWorkRecovery recover_file_work(const std::filesystem::path& database, LocalCatalogue& catalogue,
                                   const MoveFollower& follow,
                                   const operations::MetadataBackupRetentionPolicy& retention) {
    FileWorkRecovery recovery;
    const auto remember = [&recovery](core::Error error) {
        if (!recovery.error) {
            recovery.error = std::move(error);
        }
    };
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
    if (auto recovered = operations::recover_metadata_operations(journal, dependent); !recovered) {
        remember(std::move(recovered.error()));
    } else {
        for (const auto& result : *recovered) {
            if (result.outcome != operations::MetadataRecoveryOutcome::needs_reconciliation) {
                ++recovery.recovered;
            }
        }
        // A finished write keeps the file it replaced, for undo (ADR-0263),
        // within the retention policy: the newest first, by age, count and
        // size.
        if (auto maintained = operations::maintain_metadata_backups(
                journal, retention, static_cast<std::int64_t>(std::time(nullptr)));
            !maintained) {
            remember(std::move(maintained.error()));
        }
    }

    auto file_opened = persistence::SqliteFilePublicationJournal::open(database);
    if (!file_opened) {
        remember(std::move(file_opened.error()));
        return recovery;
    }
    auto file_journal = std::move(*file_opened);
    const operations::FilePublicationDependentStateCommitter moved =
        [&follow](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
        auto document = published_document(result);
        if (!document) {
            return std::unexpected(std::move(document.error()));
        }
        return follow(result, *document ? &**document : nullptr);
    };
    const auto count =
        [&recovery, &remember](
            core::Result<std::vector<operations::FilePublicationRecoveryResult>> recovered) {
            if (!recovered) {
                remember(std::move(recovered.error()));
                return;
            }
            recovery.recovered +=
                static_cast<std::size_t>(std::ranges::count_if(*recovered, [](const auto& result) {
                    return result.outcome !=
                           operations::FilePublicationRecoveryOutcome::needs_reconciliation;
                }));
        };
    count(operations::recover_same_filesystem_publications(file_journal, moved));
    count(operations::recover_cross_filesystem_publications(file_journal, moved));
    // ADR-0263: an undo a crash interrupted is finished; the sources
    // publications kept, retained as metadata backups are.
    count(operations::recover_publication_undos(file_journal, moved));
    if (auto maintained = operations::maintain_publication_backups(
            file_journal, retention, static_cast<std::int64_t>(std::time(nullptr)));
        !maintained) {
        remember(std::move(maintained.error()));
    }
    // ADR-0266: every undo copy where the engine keeps them -- those kept
    // beside files before, and any a crash left half moved.
    if (auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database);
        metadata_journal) {
        if (auto kept = operations::keep_undo_copies_in_place(*metadata_journal, file_journal);
            !kept) {
            remember(std::move(kept.error()));
        }
    }
    return recovery;
}

namespace {

constexpr std::string_view retention_key = "backups.retention";
constexpr std::string_view location_key = "backups.location";

// The place a document names, checked: {place, folder?}.
[[nodiscard]] core::Result<Json> location_of(const Json& document) {
    const auto place = document.value("place", std::string{"engine"});
    if (place == "engine" || place == "beside") {
        return Json{{"place", place}};
    }
    if (place != "folder") {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "place is engine, folder or beside",
                                           .context = {{.key = "param", .value = "place"}}});
    }
    const auto folder = document.find("folder");
    auto raw = folder != document.end() && folder->is_string()
                   ? protocol::decode_raw_path(folder->get<std::string>())
                   : core::Result<std::string>{std::unexpected(core::Error{
                         .code = core::ErrorCode::invalid_argument, .message = {}, .context = {}})};
    if (!raw || !std::filesystem::path{*raw}.is_absolute()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a folder is an absolute path",
                                           .context = {{.key = "param", .value = "folder"}}});
    }
    return Json{{"place", "folder"}, {"folder", protocol::encode_raw_path(*raw)}};
}

[[nodiscard]] Json stored_location(const Workspace& workspace) {
    auto stored = workspace.load_engine_state(location_key);
    if (stored && *stored) {
        auto document = Json::parse(**stored, nullptr, false);
        if (!document.is_discarded() && document.is_object()) {
            if (auto checked = location_of(document)) {
                return *checked;
            }
        }
    }
    return Json{{"place", "engine"}};
}

[[nodiscard]] std::filesystem::path folder_of(const Json& location,
                                              const std::filesystem::path& database) {
    const auto place = location.value("place", std::string{"engine"});
    if (place == "beside") {
        return {};
    }
    if (place == "folder") {
        if (auto raw = protocol::decode_raw_path(location.value("folder", std::string{}))) {
            return std::filesystem::path{*raw};
        }
    }
    return database.parent_path() / "undo";
}

// Every undo copy the journals know moved where they are kept now.
[[nodiscard]] core::Result<std::size_t> relocate_undo_copies(const std::filesystem::path& database) {
    auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database);
    if (!metadata_journal) {
        return std::unexpected(std::move(metadata_journal.error()));
    }
    auto file_journal = persistence::SqliteFilePublicationJournal::open(database);
    if (!file_journal) {
        return std::unexpected(std::move(file_journal.error()));
    }
    return operations::keep_undo_copies_in_place(*metadata_journal, *file_journal);
}
constexpr std::int64_t seconds_a_day = 24 * 60 * 60;
constexpr std::uint64_t bytes_a_gigabyte = 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] Json retention_document(const operations::MetadataBackupRetentionPolicy& policy) {
    return Json{{"max_age_days", policy.maximum_age_seconds / seconds_a_day},
                {"max_writes", policy.maximum_entries},
                {"max_gigabytes", policy.maximum_total_bytes / bytes_a_gigabyte}};
}

// The limits a document names over `base`; an out-of-range one refused.
[[nodiscard]] core::Result<operations::MetadataBackupRetentionPolicy>
retention_of(const Json& document, operations::MetadataBackupRetentionPolicy base) {
    const auto limit = [&document](const char* name, const std::int64_t most)
        -> core::Result<std::optional<std::int64_t>> {
        const auto found = document.find(name);
        if (found == document.end()) {
            return std::optional<std::int64_t>{};
        }
        if (!found->is_number_integer() || found->get<std::int64_t>() < 0 ||
            found->get<std::int64_t>() > most) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = std::string{name} + " is a whole number from 0 to " +
                           std::to_string(most),
                .context = {{.key = "param", .value = name}}});
        }
        return std::optional{found->get<std::int64_t>()};
    };
    auto days = limit("max_age_days", 3'650);
    auto writes = limit("max_writes", 100'000);
    auto gigabytes = limit("max_gigabytes", 100'000);
    for (auto* checked : {&days, &writes, &gigabytes}) {
        if (!*checked) {
            return std::unexpected(checked->error());
        }
    }
    if (days->has_value()) {
        base.maximum_age_seconds = **days * seconds_a_day;
    }
    if (writes->has_value()) {
        base.maximum_entries = static_cast<std::size_t>(**writes);
    }
    if (gigabytes->has_value()) {
        base.maximum_total_bytes = static_cast<std::uint64_t>(**gigabytes) * bytes_a_gigabyte;
    }
    return base;
}

// Both kinds of backup brought within the policy, now.
[[nodiscard]] core::Result<void>
apply_retention(const std::filesystem::path& database,
                const operations::MetadataBackupRetentionPolicy& policy) {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database);
    if (!metadata_journal) {
        return std::unexpected(std::move(metadata_journal.error()));
    }
    if (auto kept = operations::maintain_metadata_backups(*metadata_journal, policy, now); !kept) {
        return std::unexpected(std::move(kept.error()));
    }
    auto file_journal = persistence::SqliteFilePublicationJournal::open(database);
    if (!file_journal) {
        return std::unexpected(std::move(file_journal.error()));
    }
    if (auto kept = operations::maintain_publication_backups(*file_journal, policy, now); !kept) {
        return std::unexpected(std::move(kept.error()));
    }
    return {};
}

} // namespace

operations::MetadataBackupRetentionPolicy backup_retention(const Workspace& workspace) {
    const operations::MetadataBackupRetentionPolicy defaults{};
    auto stored = workspace.load_engine_state(retention_key);
    if (!stored || !*stored) {
        return defaults;
    }
    auto document = Json::parse(**stored, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return defaults;
    }
    auto policy = retention_of(document, defaults);
    return policy ? *policy : defaults;
}

std::filesystem::path undo_copy_folder(const Workspace& workspace,
                                       const std::filesystem::path& database) {
    return folder_of(stored_location(workspace), database);
}

void register_backup_methods(protocol::Dispatcher& dispatcher, std::filesystem::path database,
                             Workspace& workspace) {
    const auto described = [database](Json location) {
        location["kept_in"] = protocol::encode_raw_path(folder_of(location, database).native());
        return location;
    };
    dispatcher.on("backups.location", [&workspace, described](const Json&) -> core::Result<Json> {
        return described(stored_location(workspace));
    });
    dispatcher.on("backups.set_location",
                  [&workspace, database, described](const Json& params) -> core::Result<Json> {
                      auto location = location_of(params);
                      if (!location) {
                          return std::unexpected(std::move(location.error()));
                      }
                      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                      if (auto saved =
                              workspace.save_engine_state(location_key, location->dump(), now_ms);
                          !saved) {
                          return std::unexpected(std::move(saved.error()));
                      }
                      operations::set_undo_copy_folder(folder_of(*location, database));
                      // Moved at once: what the person chose holds for the copies there are.
                      if (auto moved = relocate_undo_copies(database); !moved) {
                          std::cerr << "melodyd: undo copies not all moved: "
                                    << moved.error().message << "\n";
                      }
                      return described(*location);
                  });
    dispatcher.on("backups.retention", [&workspace](const Json&) -> core::Result<Json> {
        return retention_document(backup_retention(workspace));
    });
    dispatcher.on("backups.set_retention",
                  [&workspace, database](const Json& params) -> core::Result<Json> {
                      auto policy = retention_of(params, backup_retention(workspace));
                      if (!policy) {
                          return std::unexpected(std::move(policy.error()));
                      }
                      const auto document = retention_document(*policy);
                      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();
                      if (auto saved =
                              workspace.save_engine_state(retention_key, document.dump(), now_ms);
                          !saved) {
                          return std::unexpected(std::move(saved.error()));
                      }
                      // Applied at once: a lower limit frees its space now.
                      if (auto applied = apply_retention(database, *policy); !applied) {
                          std::cerr << "melodyd: backups not brought within the new limits: "
                                    << applied.error().message << "\n";
                      }
                      return document;
                  });
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
            if (!database.empty()) {
                auto opened = persistence::SqliteFilePublicationJournal::open(database);
                auto recent =
                    opened ? opened->load_recent()
                           : core::Result<std::vector<operations::FilePublicationJournalRecord>>{
                                 std::unexpected(opened.error())};
                if (!recent) {
                    if (!error) {
                        error = std::move(recent.error());
                    }
                } else {
                    for (const auto& record : *recent) {
                        if (record.state !=
                            operations::FilePublicationJournalState::needs_reconciliation) {
                            continue;
                        }
                        interrupted.push_back(
                            Json{{"id", record.id.to_string()},
                                 {"path", protocol::encode_raw_path(record.source_raw_path)},
                                 {"target", protocol::encode_raw_path(record.target_raw_path)},
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
        std::vector<std::string> raw_paths;
        raw_paths.reserve(paths->size());
        for (const auto& encoded : *paths) {
            if (!encoded.is_string()) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "a path is not an encoded path",
                                .context = {{.key = "param", .value = "paths"}}});
            }
            auto raw_path = protocol::decode_raw_path(encoded.get<std::string>());
            if (!raw_path) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "a path is not an encoded path",
                                .context = {{.key = "param", .value = "paths"}}});
            }
            raw_paths.push_back(std::move(*raw_path));
        }
        // A batch is read by a few threads at once: a file's tags are a seek
        // and a small read, and one after another a disk spends most of a
        // batch waiting. Bounded, as every pool here is.
        std::vector<Json> read(raw_paths.size());
        std::atomic<std::size_t> next{0};
        const auto work = [&] {
            for (auto index = next++; index < raw_paths.size(); index = next++) {
                const auto& raw_path = raw_paths[index];
                auto result = metadata::read_local_metadata(raw_path);
                if (result) {
                    read[index] = Json{{"read", wire::encode(*result)}};
                    continue;
                }
                Json failed = Json::object();
                failed["error"] = wire::encode(result.error());
                failed["revision"] = nullptr;
                if (result.error().code == core::ErrorCode::unsupported) {
                    if (auto revision = core::observe_local_source_revision(raw_path)) {
                        failed["revision"] = wire::encode(*revision);
                    }
                }
                read[index] = std::move(failed);
            }
        };
        const auto threads = std::min<std::size_t>(
            {metadata_read_threads, raw_paths.size(),
             std::max(1U, std::thread::hardware_concurrency())});
        {
            std::vector<std::jthread> pool;
            for (std::size_t thread = 1; thread < threads; ++thread) {
                pool.emplace_back(work);
            }
            work();
        }
        auto files = Json::array();
        for (auto& file : read) {
            files.push_back(std::move(file));
        }
        return Json{{"files", std::move(files)}};
    });
}

namespace {

[[nodiscard]] core::Error bad_param(const std::string& message, const std::string& param) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = message,
                       .context = {{.key = "param", .value = param}}};
}

[[nodiscard]] std::uint64_t maximum_bytes_of(const Json& params) {
    constexpr std::uint64_t default_maximum = 16U * 1024U * 1024U;
    const auto asked = params.value("maximum_bytes", default_maximum);
    return std::min<std::uint64_t>(asked == 0U ? default_maximum : asked,
                                   operations::maximum_fittable_artwork_bytes);
}

[[nodiscard]] std::string sha256_hex(const std::string& bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string text;
    for (unsigned int index = 0; index < length; ++index) {
        text.push_back(hex[digest[index] >> 4U]);
        text.push_back(hex[digest[index] & 0x0FU]);
    }
    return text;
}

[[nodiscard]] core::Result<std::string> path_param(const Json& params, const std::string& name) {
    const auto encoded = params.value(name, std::string{});
    auto raw_path = protocol::decode_raw_path(encoded);
    if (encoded.empty() || !raw_path) {
        return std::unexpected(bad_param("an encoded path is required", name));
    }
    return raw_path;
}

} // namespace

namespace {

// An image a client handed over, kept in `staging` under its content's name
// and inspected there. Kept before, it is left as it is: a plan made from the
// first staging names its revision, and touching it would make that stale.
[[nodiscard]] core::Result<Json> keep_staged(const std::filesystem::path& staging,
                                             const std::string& image_bytes) {
    if (image_bytes.size() > operations::maximum_fittable_artwork_bytes) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "the image is too large",
                                           .context = {}});
    }
    const auto* data = reinterpret_cast<const unsigned char*>(image_bytes.data());
    const auto inspected =
        metadata::inspect_encoded_image_bytes(std::span{data, image_bytes.size()});
    if (!inspected) {
        return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                           .message = "only PNG and JPEG images are taken",
                                           .context = {}});
    }
    std::error_code ignored;
    std::filesystem::create_directories(staging, ignored);
    const auto target = staging / (sha256_hex(image_bytes) +
                                   (inspected->mime_type == "image/png" ? ".png" : ".jpg"));
    if (!std::filesystem::exists(target, ignored)) {
        const auto partial = target.string() + ".partial";
        const auto descriptor =
            ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const auto written = descriptor < 0
                                 ? ssize_t{-1}
                                 : ::write(descriptor, image_bytes.data(), image_bytes.size());
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        std::error_code renamed;
        std::filesystem::rename(partial, target, renamed);
        if (written != static_cast<ssize_t>(image_bytes.size()) || renamed) {
            std::filesystem::remove(partial, ignored);
            return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                               .message = "could not keep the image",
                                               .context = {}});
        }
    }
    // Staged before, it is left as it is: a plan made from the first
    // staging names its revision, and touching it would make that stale.
    auto image = metadata::read_artwork_image_file(
        target.string(), operations::maximum_fittable_artwork_bytes);
    if (!image) {
        return std::unexpected(std::move(image.error()));
    }
    return Json{{"image", wire::encode(*image)}};
}

// A large image arrives in parts: each appended to its upload's file, in
// order; the last makes it a staged image like any other.
constexpr std::string_view upload_prefix = "upload-";

} // namespace

void register_artwork_methods(protocol::Dispatcher& dispatcher, std::filesystem::path staging) {
    dispatcher.on("artwork.inventory", [](const Json& params) -> core::Result<Json> {
        const auto paths = params.find("paths");
        if (paths == params.end() || !paths->is_array() || paths->size() > metadata_read_limit) {
            return std::unexpected(bad_param(
                "up to " + std::to_string(metadata_read_limit) + " paths are required", "paths"));
        }
        auto policy = metadata::default_artwork_inventory_policy();
        if (const auto given = params.find("policy"); given != params.end() && !given->is_null()) {
            auto decoded = wire::decode_inventory_policy(*given);
            if (!decoded) {
                return std::unexpected(std::move(decoded.error()));
            }
            policy = std::move(*decoded);
        }
        auto files = Json::array();
        for (const auto& encoded : *paths) {
            if (!encoded.is_string()) {
                return std::unexpected(bad_param("a path is not an encoded path", "paths"));
            }
            auto raw_path = protocol::decode_raw_path(encoded.get<std::string>());
            if (!raw_path) {
                return std::unexpected(bad_param("a path is not an encoded path", "paths"));
            }
            Json file = Json::object();
            if (auto inventory = metadata::read_local_artwork_inventory(*raw_path, policy)) {
                file["inventory"] = wire::encode(*inventory);
            } else {
                file["error"] = wire::encode(inventory.error());
            }
            files.push_back(std::move(file));
        }
        return Json{{"files", std::move(files)}};
    });

    dispatcher.on("artwork.image_file", [](const Json& params) -> core::Result<Json> {
        auto raw_path = path_param(params, "path");
        if (!raw_path) {
            return std::unexpected(std::move(raw_path.error()));
        }
        auto image = metadata::read_artwork_image_file(*raw_path, maximum_bytes_of(params));
        if (!image) {
            return std::unexpected(std::move(image.error()));
        }
        return Json{{"image", wire::encode(*image)}};
    });

    dispatcher.on("artwork.image_bytes", [](const Json& params) -> core::Result<Json> {
        const auto given = params.find("image");
        if (given == params.end()) {
            return std::unexpected(bad_param("an image is required", "image"));
        }
        auto image = wire::decode_image_file(*given);
        if (!image) {
            return std::unexpected(std::move(image.error()));
        }
        auto bytes = metadata::read_artwork_image_bytes(*image, maximum_bytes_of(params));
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        return Json{{"bytes", protocol::encode_raw_path(std::string{
                                  reinterpret_cast<const char*>(bytes->data()), bytes->size()})}};
    });

    dispatcher.on("artwork.destination", [](const Json& params) -> core::Result<Json> {
        auto raw_path = path_param(params, "path");
        if (!raw_path) {
            return std::unexpected(std::move(raw_path.error()));
        }
        auto existing = operations::local_artwork_file_access().destination(*raw_path, {});
        if (!existing) {
            return std::unexpected(std::move(existing.error()));
        }
        return Json{{"image", *existing ? wire::encode(**existing) : Json()}};
    });

    dispatcher.on(
        "artwork.stage", [staging](const Json& params) -> core::Result<Json> {
            const auto encoded = params.value("bytes", std::string{});
            auto bytes = protocol::decode_raw_path(encoded);
            if (encoded.empty() || !bytes || bytes->empty()) {
                return std::unexpected(bad_param("encoded image bytes are required", "bytes"));
            }
            return keep_staged(staging, *bytes);
        });

    dispatcher.on(
        "artwork.stage_part", [staging](const Json& params) -> core::Result<Json> {
            const auto upload = params.value("upload", std::string{});
            if (upload.size() < 16U || upload.size() > 64U ||
                !std::ranges::all_of(upload, [](const char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) {
                return std::unexpected(bad_param("an upload is named by hex digits", "upload"));
            }
            const auto offset = params.value("offset", std::uint64_t{0});
            auto bytes = protocol::decode_raw_path(params.value("bytes", std::string{}));
            if (!bytes) {
                return std::unexpected(bad_param("encoded image bytes are required", "bytes"));
            }
            std::error_code ignored;
            std::filesystem::create_directories(staging, ignored);
            const auto part = staging / (std::string{upload_prefix} + upload + ".part");
            const auto held = std::filesystem::exists(part, ignored)
                                  ? std::filesystem::file_size(part, ignored)
                                  : std::uintmax_t{0};
            // In order, and never past what any image may be: a part out of
            // place starts nothing over, it is refused.
            if (offset != held) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "the upload holds " + std::to_string(held) + " bytes",
                    .context = {}});
            }
            if (held + bytes->size() > operations::maximum_fittable_artwork_bytes) {
                std::filesystem::remove(part, ignored);
                return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                                   .message = "the image is too large",
                                                   .context = {}});
            }
            {
                const auto descriptor = ::open(part.c_str(),
                                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
                const auto written = descriptor < 0
                                         ? ssize_t{-1}
                                         : ::write(descriptor, bytes->data(), bytes->size());
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
                if (written != static_cast<ssize_t>(bytes->size())) {
                    std::filesystem::remove(part, ignored);
                    return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                                       .message = "could not keep the image",
                                                       .context = {}});
                }
            }
            if (!params.value("last", false)) {
                return Json{{"received", held + bytes->size()}};
            }
            std::string whole;
            {
                std::ifstream input{part, std::ios::binary};
                whole.assign(std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{});
            }
            std::filesystem::remove(part, ignored);
            if (whole.empty()) {
                return std::unexpected(bad_param("encoded image bytes are required", "bytes"));
            }
            return keep_staged(staging, whole);
        });
}

void clean_artwork_staging(const std::filesystem::path& staging) {
    std::error_code error;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::days{7};
    for (const auto& entry : std::filesystem::directory_iterator{staging, error}) {
        std::error_code ignored;
        if (entry.is_regular_file(ignored) && entry.last_write_time(ignored) < cutoff) {
            std::filesystem::remove(entry.path(), ignored);
        }
    }
}

} // namespace trackknife::engine
