// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/rating_tags.hpp"

#include "trackknife/engine/workspace.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::string_view rating_field = metadata::fmps_rating_field;
constexpr std::string_view enabled_key = "ratings.write-tags";
constexpr std::string_view imported_key = "ratings.imported-from-tags";
constexpr std::string_view backup_key = "ratings.backup-tag";

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

RatingTags::RatingTags(std::filesystem::path database, LocalCatalogue& catalogue,
                       Workspace& workspace)
    : database_(std::move(database)), catalogue_(catalogue), workspace_(workspace) {
    if (auto stored = workspace_.load_engine_state(enabled_key); stored && *stored) {
        enabled_ = **stored == "1";
    }
    if (auto stored = workspace_.load_engine_state(backup_key); stored && *stored) {
        backup_tag_ = **stored;
    }
    // Ratings in files are imported as they are read. A library read before
    // that is caught up once, from what it read.
    if (auto done = workspace_.load_engine_state(imported_key); done && !*done) {
        if (auto imported = catalogue_.import_indexed_tag_ratings()) {
            static_cast<void>(workspace_.save_engine_state(imported_key, "1", now_ms()));
            if (*imported > 0U) {
                std::cerr << "melodyd: took " << *imported << " rating(s) from the files\n";
            }
        }
    }
    worker_ = std::thread{[this] { run(); }};
}

RatingTags::~RatingTags() {
    {
        const std::lock_guard guard{mutex_};
        stopping_ = true;
        cancellation_.request_cancellation();
    }
    changed_.notify_all();
    worker_.join();
}

bool RatingTags::enabled() const {
    const std::lock_guard guard{mutex_};
    return enabled_;
}

core::Result<void> RatingTags::set_enabled(const bool enabled) {
    if (auto saved = workspace_.save_engine_state(enabled_key, enabled ? "1" : "0", now_ms());
        !saved) {
        return saved;
    }
    {
        const std::lock_guard guard{mutex_};
        const auto was = enabled_;
        enabled_ = enabled;
        if (was != enabled) {
            std::cerr << "melodyd: ratings are " << (enabled ? "now" : "no longer")
                      << " written into the files\n";
        }
        if (!enabled) {
            // What is waiting is not written; what is in the files stays.
            queue_.clear();
        } else if (!was) {
            queue_.push_back(Work{.hash = std::nullopt, .rating = 0U});
        }
    }
    changed_.notify_all();
    return {};
}

std::string RatingTags::backup_tag() const {
    const std::lock_guard guard{mutex_};
    return backup_tag_;
}

core::Result<void> RatingTags::set_backup_tag(std::string tag) {
    if (!tag.empty()) {
        if (auto problem = metadata::rating_backup_tag_problem(tag)) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = std::move(*problem),
                                               .context = {{.key = "param", .value = "backup_tag"}}});
        }
    }
    if (tag == backup_tag()) {
        return {};
    }
    if (auto saved = workspace_.save_engine_state(backup_key, tag, now_ms()); !saved) {
        return saved;
    }
    {
        const std::lock_guard guard{mutex_};
        backup_tag_ = tag;
        std::cerr << "melodyd: ratings are "
                  << (tag.empty() ? std::string{"no longer copied into a backup tag"}
                                  : "also copied into " + tag)
                  << "\n";
        // Every rated track once, now that the copy goes somewhere new.
        if (enabled_ && !tag.empty()) {
            queue_.push_back(Work{.hash = std::nullopt, .rating = 0U});
        }
    }
    changed_.notify_all();
    return {};
}

void RatingTags::rated(const std::string& hash, const bool album, const unsigned rating) {
    if (album) {
        return;
    }
    {
        const std::lock_guard guard{mutex_};
        if (!enabled_) {
            return;
        }
        // Rated again before it was written: only the last one is.
        const auto queued = std::ranges::find(queue_, std::optional{hash}, &Work::hash);
        if (queued != queue_.end()) {
            queued->rating = rating;
            return;
        }
        queue_.push_back(Work{.hash = hash, .rating = rating});
    }
    changed_.notify_all();
}

metadata::PlainRatingScale RatingTags::plain_scale() const {
    auto stored = workspace_.load_engine_state(persistence::plain_rating_scale_state_key);
    return stored && *stored ? metadata::plain_rating_scale_named(**stored).value_or(
                                   metadata::PlainRatingScale::off)
                             : metadata::PlainRatingScale::off;
}

core::Result<void> RatingTags::set_plain_scale(const metadata::PlainRatingScale scale) {
    if (scale == plain_scale()) {
        return {};
    }
    if (auto saved =
            workspace_.save_engine_state(persistence::plain_rating_scale_state_key,
                                         metadata::plain_rating_scale_name(scale), now_ms());
        !saved) {
        return saved;
    }
    std::cerr << "melodyd: RATING tags are read on the scale "
              << metadata::plain_rating_scale_name(scale) << "\n";
    auto imported = catalogue_.import_indexed_tag_ratings();
    if (!imported) {
        return std::unexpected(std::move(imported.error()));
    }
    return {};
}

std::size_t RatingTags::pending() const {
    const std::lock_guard guard{mutex_};
    return queue_.size() + in_flight_;
}

void RatingTags::wait_idle() {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return (queue_.empty() && in_flight_ == 0U) || stopping_; });
}

void RatingTags::run() {
    const auto token = cancellation_.token();
    while (true) {
        Work work;
        {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }
        std::vector<std::pair<std::string, unsigned>> files;
        if (work.hash) {
            if (auto paths = catalogue_.rated_paths(*work.hash)) {
                for (auto& path : *paths) {
                    files.emplace_back(std::move(path), work.rating);
                }
            }
        } else if (auto rated = catalogue_.rated_tracks(token)) {
            files = std::move(*rated);
        }
        for (const auto& [path, rating] : files) {
            if (token.is_cancellation_requested()) {
                break;
            }
            std::string backup;
            {
                // Turned off meanwhile: nothing more is written.
                const std::lock_guard guard{mutex_};
                if (!enabled_) {
                    break;
                }
                backup = backup_tag_;
            }
            if (auto written = write(database_, catalogue_, path, rating, token, backup);
                !written) {
                std::cerr << "melodyd: could not write the rating into "
                          << core::display_raw_path(path) << ": " << written.error().message
                          << "\n";
            }
        }
        {
            const std::lock_guard guard{mutex_};
            --in_flight_;
        }
        changed_.notify_all();
    }
}

core::Result<bool> RatingTags::write(const std::filesystem::path& database,
                                     LocalCatalogue& catalogue, const std::string& raw_path,
                                     const unsigned rating,
                                     const core::CancellationToken& cancellation,
                                     const std::string_view backup_tag) {
    auto read = metadata::read_local_metadata(raw_path, cancellation);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    if (!read->capabilities.fields_writable) {
        return false;
    }
    const std::vector<std::string> wanted =
        rating == 0U ? std::vector<std::string>{} : std::vector{metadata::fmps_rating_text(rating)};
    // The copy: the plain 0-10 number (ADR-0245).
    const std::vector<std::string> copied =
        rating == 0U ? std::vector<std::string>{} : std::vector{std::to_string(rating)};
    const bool rating_differs = read->document.effective_values(rating_field) != wanted;
    const bool copy_differs =
        !backup_tag.empty() && read->document.effective_values(backup_tag) != copied;
    if (!rating_differs && !copy_differs) {
        return false;
    }
    auto selection = metadata::StagedMetadataSelection::create(
        {metadata::StagedMetadataSource{.raw_path = raw_path,
                                        .source_revision = read->source_revision,
                                        .baseline = read->document}});
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }
    metadata::StagedMetadataPatchSet patches;
    const auto stage = [&](const std::size_t field,
                           const std::vector<std::string>& values) -> core::Result<void> {
        auto staged = values.empty() ? patches.remove_field(*selection, 0U, field)
                                     : patches.replace_values(*selection, 0U, field, values);
        return staged ? core::Result<void>{} : std::unexpected(std::move(staged.error()));
    };
    if (rating_differs) {
        // Written under exactly this name, as other players look for it.
        auto field = selection->ensure_exact_native_field(rating_field, rating_field);
        if (!field) {
            return std::unexpected(std::move(field.error()));
        }
        if (auto staged = stage(*field, wanted); !staged) {
            return std::unexpected(std::move(staged.error()));
        }
    }
    if (copy_differs) {
        // An official tag is written as each format writes it -- COMMENT as
        // MP3's COMM frame -- and any other name exactly as it is spelled.
        auto field = metadata::official_tag_name(backup_tag)
                         ? (selection->field_index(backup_tag)
                                ? core::Result<std::size_t>{*selection->field_index(backup_tag)}
                                : selection->ensure_missing_field(backup_tag, backup_tag))
                         : selection->ensure_exact_native_field(backup_tag, backup_tag);
        if (!field) {
            return std::unexpected(std::move(field.error()));
        }
        if (auto staged = stage(*field, copied); !staged) {
            return std::unexpected(std::move(staged.error()));
        }
    }
    auto plan = metadata::revalidate_metadata_write_plan(*selection, patches, cancellation);
    if (!plan) {
        return std::unexpected(std::move(plan.error()));
    }
    if (!plan->ready() || plan->sources.size() != 1U) {
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "the file cannot take the rating now",
                                           .context = {}});
    }
    auto opened = persistence::SqliteMetadataOperationJournal::open(database);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    auto journal = std::move(*opened);
    const operations::MetadataDependentStateCommitter dependent =
        [&catalogue](const operations::MetadataCommitResult& result) -> core::Result<void> {
        auto refreshed = catalogue.refresh({result.source_raw_path});
        return refreshed ? core::Result<void>{} : std::unexpected(std::move(refreshed.error()));
    };
    auto committed = operations::commit_flac_metadata_source(plan->sources.front(), journal,
                                                             dependent, cancellation);
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return true;
}

void register_rating_tag_methods(protocol::Dispatcher& dispatcher, RatingTags& tags) {
    const auto state = [&tags] {
        return Json{
            {"write_tags", tags.enabled()},
            {"rating_scale", std::string{metadata::plain_rating_scale_name(tags.plain_scale())}},
            {"backup_tag", tags.backup_tag()},
            {"pending", tags.pending()}};
    };
    dispatcher.on("ratings.tags", [state](const Json&) -> core::Result<Json> { return state(); });
    dispatcher.on("ratings.set_tags", [&tags, state](const Json& params) -> core::Result<Json> {
        const auto wanted = params.find("write_tags");
        const auto scale = params.find("rating_scale");
        const auto backup = params.find("backup_tag");
        if (backup != params.end() && !backup->is_string()) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::invalid_argument,
                            .message = "backup_tag must be a tag name, or empty for none",
                            .context = {{.key = "param", .value = "backup_tag"}}});
        }
        if ((wanted == params.end() && scale == params.end() && backup == params.end()) ||
            (wanted != params.end() && !wanted->is_boolean())) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::invalid_argument,
                            .message = "write_tags must be true or false",
                            .context = {{.key = "param", .value = "write_tags"}}});
        }
        std::optional<metadata::PlainRatingScale> named;
        if (scale != params.end()) {
            named = scale->is_string()
                        ? metadata::plain_rating_scale_named(scale->get<std::string>())
                        : std::nullopt;
            if (!named) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::invalid_argument,
                                .message = "rating_scale must be off, 5, 10 or 100",
                                .context = {{.key = "param", .value = "rating_scale"}}});
            }
        }
        if (wanted != params.end()) {
            if (auto set = tags.set_enabled(wanted->get<bool>()); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        if (named) {
            if (auto set = tags.set_plain_scale(*named); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        if (backup != params.end()) {
            if (auto set = tags.set_backup_tag(backup->get<std::string>()); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        return state();
    });
}

} // namespace trackknife::engine
