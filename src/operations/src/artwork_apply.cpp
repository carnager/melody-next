// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/artwork_apply.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <ranges>
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
