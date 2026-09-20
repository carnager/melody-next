// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/artwork_apply.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <ranges>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_apply_parallelism = 8U;

[[nodiscard]] core::Error apply_error(const core::ErrorCode code, std::string message,
                                      const std::string& raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        result.context.push_back({.key = "source", .value = core::escape_raw_path(raw_path)});
    }
    return result;
}

} // namespace

core::Result<metadata::ArtworkWritePlan>
plan_artwork_storage(const std::vector<metadata::ArtworkWritePlanIntent>& intents,
                     const metadata::ArtworkStoragePolicy& policy,
                     const core::CancellationToken& cancellation) {
    if (intents.empty() || intents.size() > 100'000U)
        return std::unexpected(apply_error(core::ErrorCode::invalid_argument,
                                           "Cover review requires 1–100000 intents"));
    const auto& name = policy.folder_image_name;
    if ((!policy.embed && !policy.write_folder_image) || policy.fetch_source != "coverartarchive" ||
        (policy.write_folder_image && (name.empty() || name == "." || name == ".." ||
                                       name.find_first_of("/\\") != std::string::npos ||
                                       name.find('\0') != std::string::npos))) {
        return std::unexpected(apply_error(
            core::ErrorCode::invalid_argument,
            "Choose embedding and/or a folder image with a plain filename in Cover settings"));
    }
    core::Result<metadata::ArtworkWritePlan> plan = metadata::ArtworkWritePlan{};
    if (policy.embed) {
        plan = metadata::revalidate_artwork_write_plan(intents, cancellation);
        if (!plan || !plan->ready())
            return plan;
    } else {
        // Folder-only publication does not require an embedded writer. Build
        // revision-qualified front-image inputs without implying tag capability.
        plan->logical_intent_count = intents.size();
        for (const auto& intent : intents) {
            if (cancellation.is_cancellation_requested())
                return std::unexpected(
                    apply_error(core::ErrorCode::cancelled, "Cover review cancelled"));
            if (intent.kind == metadata::ArtworkWritePlanIntentKind::remove)
                continue;
            auto role = intent.added_role;
            if (intent.kind == metadata::ArtworkWritePlanIntentKind::replace) {
                auto inventory = metadata::read_local_artwork_inventory(
                    intent.raw_media_path, metadata::default_artwork_inventory_policy(),
                    cancellation);
                if (!inventory)
                    return std::unexpected(inventory.error());
                const auto target = std::ranges::find_if(inventory->items, [&](const auto& item) {
                    return item.provenance == metadata::ArtworkProvenance::embedded &&
                           item.source_ordinal == intent.target_ordinal;
                });
                if (target == inventory->items.end() ||
                    target->content_fingerprint != intent.expected_target_fingerprint)
                    return std::unexpected(apply_error(core::ErrorCode::conflict,
                                                       "Reviewed front cover changed",
                                                       intent.raw_media_path));
                role = target->role;
            }
            if ((intent.kind != metadata::ArtworkWritePlanIntentKind::add &&
                 intent.kind != metadata::ArtworkWritePlanIntentKind::replace) ||
                role != metadata::ArtworkRole::front ||
                (!intent.replacement_raw_path && !intent.replacement_embedded_source))
                return std::unexpected(apply_error(core::ErrorCode::unsupported,
                                                   "Folder-only mode accepts front images; enable "
                                                   "embedding for other artwork roles"));
            auto observed = core::observe_local_source_revision(intent.raw_media_path);
            if (!observed || !intent.expected_media_revision ||
                *observed != *intent.expected_media_revision)
                return std::unexpected(apply_error(core::ErrorCode::conflict,
                                                   "Cover source changed after inspection",
                                                   intent.raw_media_path));
            core::Result<metadata::ArtworkImageFile> image = std::unexpected(
                apply_error(core::ErrorCode::invalid_argument, "Missing image input"));
            if (intent.replacement_embedded_source) {
                const auto& donor = *intent.replacement_embedded_source;
                image = metadata::ArtworkImageFile{.raw_path = donor.raw_source_path,
                                                   .source_revision = donor.source_revision,
                                                   .mime_type = donor.mime_type,
                                                   .width = donor.width,
                                                   .height = donor.height,
                                                   .byte_size = donor.byte_size,
                                                   .content_fingerprint = donor.content_fingerprint,
                                                   .embedded_source_ordinal = donor.source_ordinal};
                auto verified =
                    metadata::read_artwork_image_bytes(*image, 16U * 1024U * 1024U, cancellation);
                if (!verified)
                    return std::unexpected(verified.error());
            } else {
                image = metadata::read_artwork_image_file(*intent.replacement_raw_path,
                                                          16U * 1024U * 1024U, cancellation);
            }
            if (!image)
                return std::unexpected(image.error());
            auto found = std::ranges::find(plan->sources, intent.raw_media_path,
                                           &metadata::ArtworkWritePlanSource::raw_media_path);
            if (found != plan->sources.end()) {
                if (found->change.replacement->content_fingerprint != image->content_fingerprint)
                    return std::unexpected(apply_error(core::ErrorCode::conflict,
                                                       "Different front images target one source",
                                                       intent.raw_media_path));
                if (!std::ranges::contains(found->occurrence_indexes, intent.occurrence_index))
                    found->occurrence_indexes.push_back(intent.occurrence_index);
                continue;
            }
            plan->sources.push_back({.raw_media_path = intent.raw_media_path,
                                     .occurrence_indexes = {intent.occurrence_index},
                                     .expected_media_revision = intent.expected_media_revision,
                                     .observed_media_revision = *observed,
                                     .adapter_name = "folder-image-v1",
                                     .change = {.kind = metadata::ArtworkWritePlanIntentKind::add,
                                                .target_ordinal = 0,
                                                .expected_target_fingerprint = {},
                                                .original = std::nullopt,
                                                .replacement = std::move(*image),
                                                .added_role = metadata::ArtworkRole::front,
                                                .added_description = {}},
                                     .issues = {},
                                     .additional_changes = {},
                                     .embed = false,
                                     .folder_image = std::nullopt});
        }
        if (plan->sources.empty())
            return std::unexpected(apply_error(
                core::ErrorCode::invalid_argument,
                "Folder-image deletion is not supported; select a front image to publish"));
    }
    std::map<std::string, core::ContentFingerprint> destinations;
    for (auto& source : plan->sources) {
        source.embed = policy.embed;
        if (policy.write_folder_image) {
            for (const auto& change : metadata::artwork_changes(source)) {
                const auto role = change.original ? change.original->role : change.added_role;
                if (!change.replacement || role != metadata::ArtworkRole::front)
                    continue;
                auto filename = std::filesystem::path{name};
                filename.replace_extension(change.replacement->mime_type == "image/png" ? ".png"
                                                                                        : ".jpg");
                const auto destination =
                    (std::filesystem::path{source.raw_media_path}.parent_path() / filename)
                        .native();
                const auto [found, inserted] =
                    destinations.emplace(destination, change.replacement->content_fingerprint);
                if (!inserted && found->second != change.replacement->content_fingerprint)
                    return std::unexpected(apply_error(
                        core::ErrorCode::conflict,
                        "Different front covers target the same folder image", destination));
                metadata::FolderImageWritePlan folder{.raw_path = destination,
                                                      .image = *change.replacement,
                                                      .original = std::nullopt};
                struct stat status{};
                if (::lstat(destination.c_str(), &status) == 0) {
                    if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
                        return std::unexpected(apply_error(
                            core::ErrorCode::conflict,
                            "Folder image must be a regular file with one link", destination));
                    auto original = metadata::read_artwork_image_file(
                        destination, 16U * 1024U * 1024U, cancellation);
                    if (!original)
                        return std::unexpected(original.error());
                    folder.original = std::move(*original);
                } else if (errno != ENOENT) {
                    return std::unexpected(apply_error(core::ErrorCode::io,
                                                       "Cannot inspect folder image", destination));
                }
                source.folder_image = std::move(folder);
            }
        }
        if (!source.embed && !source.folder_image)
            return std::unexpected(apply_error(core::ErrorCode::invalid_argument,
                                               "Folder-only storage requires a front cover; enable "
                                               "embedding for other roles or removal",
                                               source.raw_media_path));
    }
    return plan;
}

std::size_t ArtworkApplyResult::committed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, ArtworkApplySourceState::committed,
                                                       &ArtworkApplySourceResult::state));
}

std::size_t ArtworkApplyResult::failed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, ArtworkApplySourceState::failed,
                                                       &ArtworkApplySourceResult::state));
}

std::size_t ArtworkApplyResult::cancelled_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources, ArtworkApplySourceState::cancelled,
                                                       &ArtworkApplySourceResult::state));
}

core::Result<ArtworkApplyResult> apply_artwork_write_plan(
    const metadata::ArtworkWritePlan& plan, const ArtworkApplySourceCommitter& committer,
    const ArtworkApplyProgressCallback& progress, const core::CancellationToken& cancellation,
    const ArtworkApplyOptions& options) {
    if (!plan.ready() || !committer || options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_apply_parallelism) {
        return std::unexpected(apply_error(
            core::ErrorCode::invalid_argument,
            "artwork Apply requires an entirely ready plan, a committer, and 1–8 workers"));
    }

    std::unordered_set<std::string> paths;
    for (const auto& source : plan.sources) {
        if (!paths.insert(source.raw_media_path).second) {
            return std::unexpected(apply_error(core::ErrorCode::invalid_argument,
                                               "artwork Apply requires one atomic plan per file",
                                               source.raw_media_path));
        }
    }
    ArtworkApplyResult result;
    result.sources.reserve(plan.sources.size());
    for (std::size_t index = 0U; index < plan.sources.size(); ++index) {
        result.sources.push_back(ArtworkApplySourceResult{
            .source_index = index,
            .raw_path = plan.sources[index].raw_media_path,
            .state = ArtworkApplySourceState::pending,
            .commit = std::nullopt,
            .issue = std::nullopt,
        });
    }

    std::atomic_size_t next_source{0U};
    std::mutex progress_mutex;
    // Guarded by progress_mutex: incrementing the completed count and
    // delivering the update must be one step, or two workers finishing
    // together can publish their counts out of order and the final update
    // arrives with a stale total.
    std::size_t completed_sources{0U};
    const auto report = [&](const std::size_t index, const ArtworkApplySourceState state,
                            const bool terminal,
                            const std::optional<core::Error>& issue = std::nullopt) {
        const std::scoped_lock lock{progress_mutex};
        if (terminal) {
            ++completed_sources;
        }
        if (!progress) {
            return;
        }
        progress(ArtworkApplyProgress{
            .source_index = index,
            .raw_path = plan.sources[index].raw_media_path,
            .state = state,
            .completed_sources = completed_sources,
            .total_sources = plan.sources.size(),
            .issue = issue,
        });
    };
    const auto worker = [&] {
        while (!cancellation.is_cancellation_requested()) {
            const auto index = next_source.fetch_add(1U, std::memory_order_relaxed);
            if (index >= plan.sources.size()) {
                return;
            }
            auto& source_result = result.sources[index];
            if (cancellation.is_cancellation_requested()) {
                source_result.state = ArtworkApplySourceState::cancelled;
                source_result.issue =
                    apply_error(core::ErrorCode::cancelled,
                                "artwork Apply was cancelled before this source started",
                                source_result.raw_path);
            } else {
                source_result.state = ArtworkApplySourceState::running;
                report(index, source_result.state, false);
                auto committed = committer(plan.sources[index], cancellation);
                if (committed) {
                    source_result.state = ArtworkApplySourceState::committed;
                    source_result.commit = std::move(*committed);
                } else {
                    source_result.issue = std::move(committed.error());
                    source_result.state = source_result.issue->code == core::ErrorCode::cancelled
                                              ? ArtworkApplySourceState::cancelled
                                              : ArtworkApplySourceState::failed;
                }
            }
            report(index, source_result.state, true, source_result.issue);
        }
    };

    const auto worker_count = std::min(options.maximum_parallelism, plan.sources.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    for (auto& source_result : result.sources) {
        if (source_result.state != ArtworkApplySourceState::pending) {
            continue;
        }
        source_result.state = ArtworkApplySourceState::cancelled;
        source_result.issue = apply_error(core::ErrorCode::cancelled,
                                          "artwork Apply was cancelled before this source started",
                                          source_result.raw_path);
        report(source_result.source_index, source_result.state, true, source_result.issue);
    }
    result.cancellation_requested =
        cancellation.is_cancellation_requested() || result.cancelled_source_count() > 0U;
    return result;
}

} // namespace trackknife::operations
