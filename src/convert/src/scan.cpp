// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/scan.hpp"

#include "trackknife/convert/artwork.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace trackknife::convert {
namespace {

[[nodiscard]] core::Error scan_error(const core::ErrorCode code, std::string message,
                                     std::string raw_path = {}) {
    core::Error error{.code = code, .message = std::move(message), .context = {}};
    if (!raw_path.empty()) {
        error.context.push_back({.key = "path", .value = std::move(raw_path)});
    }
    return error;
}

struct ItemOutcome {
    std::optional<ConvertedAudioFile> converted;
    std::optional<core::LocalSourceRevision> revision;
    std::optional<core::Error> issue;
};

[[nodiscard]] ItemOutcome convert_item(const ConversionScanItem& item,
                                       const ConversionScanOptions& options,
                                       const ConversionProgress& item_progress,
                                       const core::CancellationToken& cancellation) {
    ItemOutcome outcome;
    auto revision_before = core::observe_local_source_revision(item.source_raw_path);
    if (!revision_before) {
        outcome.issue = std::move(revision_before.error());
        return outcome;
    }
    auto converted = convert_audio_file(
        {
            .source_raw_path = item.source_raw_path,
            .source_selection = item.selection,
            .source_range = item.range,
            .destination_raw_path = item.destination_raw_path,
            .preset = options.preset,
            .target_sample_rate = options.target_sample_rate,
            .sample_rate_cap = options.sample_rate_cap,
            .target_bit_depth = options.target_bit_depth,
            .keep_source_bit_depth = options.keep_source_bit_depth,
            .channel_policy = options.channel_policy,
            .gain_mode = options.gain_mode,
            .gain_preamp_db = options.gain_preamp_db,
            .metadata = item.metadata,
            .artwork = options.carry_artwork
                           ? resolve_conversion_artwork(item.source_raw_path, cancellation)
                           : std::nullopt,
        },
        item_progress, cancellation);
    if (!converted) {
        outcome.issue = std::move(converted.error());
        return outcome;
    }
    auto revision_after = core::observe_local_source_revision(item.source_raw_path);
    if (!revision_after || *revision_after != *revision_before) {
        // The published output came from a source that changed underneath
        // the conversion; remove it so nothing suspect survives.
        std::error_code remove_error;
        std::filesystem::remove(item.destination_raw_path, remove_error);
        outcome.issue = revision_after
                            ? scan_error(core::ErrorCode::conflict,
                                         "the source changed while it was being converted",
                                         item.source_raw_path)
                            : std::move(revision_after.error());
        return outcome;
    }
    outcome.converted = std::move(*converted);
    outcome.revision = *revision_after;
    return outcome;
}

} // namespace

std::size_t ConversionScanResult::converted_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        items, [](const auto& item) { return item.state == ConversionScanState::converted; }));
}

std::size_t ConversionScanResult::failed_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        items, [](const auto& item) { return item.state == ConversionScanState::failed; }));
}

core::Result<ConversionScanResult> scan_conversion(const std::span<const ConversionScanItem> items,
                                                   const ConversionScanOptions& options,
                                                   const ConversionScanProgressCallback& progress,
                                                   const core::CancellationToken& cancellation) {
    if (options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_conversion_parallelism) {
        return std::unexpected(scan_error(core::ErrorCode::invalid_argument,
                                          "conversion scanning requires 1-16 workers"));
    }
    ConversionScanResult result;
    result.items.reserve(items.size());
    for (const auto& item : items) {
        result.items.push_back(ConvertedItemScan{
            .item_index = item.item_index,
            .source_raw_path = item.source_raw_path,
            .destination_raw_path = item.destination_raw_path,
            .state = ConversionScanState::pending,
            .converted = std::nullopt,
            .source_revision = std::nullopt,
            .issue = std::nullopt,
        });
    }

    std::atomic_size_t next_item{0U};
    std::mutex progress_mutex;
    // Guarded by progress_mutex: the completed count and its delivery must
    // be one step so concurrent completions never publish out of order.
    std::size_t completed_items{0U};
    const auto report = [&](const std::size_t position, const ConversionScanState state,
                            const bool terminal, const std::uint64_t frames_done = 0U,
                            const std::optional<std::uint64_t> frames_total = {}) {
        const std::scoped_lock lock{progress_mutex};
        if (terminal) {
            ++completed_items;
        }
        if (!progress) {
            return;
        }
        progress(ConversionScanProgress{
            .item_index = items[position].item_index,
            .source_raw_path = items[position].source_raw_path,
            .state = state,
            .completed_items = completed_items,
            .total_items = items.size(),
            .frames_done = frames_done,
            .frames_total = frames_total,
        });
    };

    const auto worker = [&] {
        while (true) {
            const auto position = next_item.fetch_add(1U);
            if (position >= items.size()) {
                return;
            }
            auto& entry = result.items[position];
            if (cancellation.is_cancellation_requested()) {
                entry.state = ConversionScanState::cancelled;
                entry.issue = scan_error(core::ErrorCode::cancelled,
                                         "the conversion was cancelled before this item started",
                                         entry.source_raw_path);
                report(position, entry.state, true);
                continue;
            }
            entry.state = ConversionScanState::running;
            report(position, entry.state, false);
            // Throttled to roughly one within-item report per second of
            // source audio so sixteen workers never serialize on progress.
            std::uint64_t last_reported = 0U;
            const auto item_progress = [&](const std::uint64_t frames_done,
                                           const std::optional<std::uint64_t> frames_total) {
                if (frames_done - last_reported < 44'100U) {
                    return;
                }
                last_reported = frames_done;
                report(position, ConversionScanState::running, false, frames_done, frames_total);
            };
            auto outcome = convert_item(
                items[position], options,
                progress ? ConversionProgress{item_progress} : ConversionProgress{}, cancellation);
            if (outcome.issue) {
                entry.state = outcome.issue->code == core::ErrorCode::cancelled
                                  ? ConversionScanState::cancelled
                                  : ConversionScanState::failed;
                entry.issue = std::move(outcome.issue);
            } else {
                entry.state = ConversionScanState::converted;
                entry.converted = std::move(outcome.converted);
                entry.source_revision = outcome.revision;
            }
            report(position, entry.state, true);
        }
    };
    {
        const auto worker_count =
            std::min(options.maximum_parallelism, std::max<std::size_t>(items.size(), 1U));
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers.emplace_back(worker);
        }
    }
    result.cancellation_requested = cancellation.is_cancellation_requested();
    return result;
}

} // namespace trackknife::convert
