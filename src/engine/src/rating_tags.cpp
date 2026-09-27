// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/rating_tags.hpp"

#include "trackknife/engine/workspace.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"
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

constexpr std::string_view rating_field = "FMPS_RATING";
constexpr std::string_view enabled_key = "ratings.write-tags";

// The freedesktop spelling: the rating over ten, one decimal.
[[nodiscard]] std::string fmps_value(const unsigned rating) {
    std::array<char, 8> text{};
    std::snprintf(text.data(), text.size(), "%.1f", static_cast<double>(rating) / 10.0);
    return text.data();
}

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
            {
                // Turned off meanwhile: nothing more is written.
                const std::lock_guard guard{mutex_};
                if (!enabled_) {
                    break;
                }
            }
            if (auto written = write(database_, catalogue_, path, rating, token); !written) {
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
                                     const core::CancellationToken& cancellation) {
    auto read = metadata::read_local_metadata(raw_path, cancellation);
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    if (!read->capabilities.fields_writable) {
        return false;
    }
    const std::vector<std::string> wanted =
        rating == 0U ? std::vector<std::string>{} : std::vector{fmps_value(rating)};
    if (read->document.effective_values(rating_field) == wanted) {
        return false;
    }
    auto selection = metadata::StagedMetadataSelection::create(
        {metadata::StagedMetadataSource{.raw_path = raw_path,
                                        .source_revision = read->source_revision,
                                        .baseline = read->document}});
    if (!selection) {
        return std::unexpected(std::move(selection.error()));
    }
    // Written under exactly this name, as other players look for it.
    auto field = selection->ensure_exact_native_field(rating_field, rating_field);
    if (!field) {
        return std::unexpected(std::move(field.error()));
    }
    metadata::StagedMetadataPatchSet patches;
    auto staged = rating == 0U ? patches.remove_field(*selection, 0U, *field)
                               : patches.replace_values(*selection, 0U, *field, wanted);
    if (!staged) {
        return std::unexpected(std::move(staged.error()));
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
        return Json{{"write_tags", tags.enabled()}, {"pending", tags.pending()}};
    };
    dispatcher.on("ratings.tags", [state](const Json&) -> core::Result<Json> { return state(); });
    dispatcher.on("ratings.set_tags", [&tags, state](const Json& params) -> core::Result<Json> {
        const auto wanted = params.find("write_tags");
        if (wanted == params.end() || !wanted->is_boolean()) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::invalid_argument,
                            .message = "write_tags must be true or false",
                            .context = {{.key = "param", .value = "write_tags"}}});
        }
        if (auto set = tags.set_enabled(wanted->get<bool>()); !set) {
            return std::unexpected(std::move(set.error()));
        }
        return state();
    });
}

} // namespace trackknife::engine
