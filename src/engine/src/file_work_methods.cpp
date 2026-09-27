// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/file_work_methods.hpp"

#include "trackknife/engine/file_work_wire.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
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
        "artwork.stage", [staging = std::move(staging)](const Json& params) -> core::Result<Json> {
            const auto encoded = params.value("bytes", std::string{});
            auto bytes = protocol::decode_raw_path(encoded);
            if (encoded.empty() || !bytes || bytes->empty()) {
                return std::unexpected(bad_param("encoded image bytes are required", "bytes"));
            }
            if (bytes->size() > operations::maximum_fittable_artwork_bytes) {
                return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                                   .message = "the image is too large",
                                                   .context = {}});
            }
            const auto* data = reinterpret_cast<const unsigned char*>(bytes->data());
            const auto inspected =
                metadata::inspect_encoded_image_bytes(std::span{data, bytes->size()});
            if (!inspected) {
                return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                                   .message = "only PNG and JPEG images are taken",
                                                   .context = {}});
            }
            std::error_code ignored;
            std::filesystem::create_directories(staging, ignored);
            const auto target = staging / (sha256_hex(*bytes) +
                                           (inspected->mime_type == "image/png" ? ".png" : ".jpg"));
            if (!std::filesystem::exists(target, ignored)) {
                const auto partial = target.string() + ".partial";
                const auto descriptor =
                    ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                const auto written = descriptor < 0
                                         ? ssize_t{-1}
                                         : ::write(descriptor, bytes->data(), bytes->size());
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
                std::error_code renamed;
                std::filesystem::rename(partial, target, renamed);
                if (written != static_cast<ssize_t>(bytes->size()) || renamed) {
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
