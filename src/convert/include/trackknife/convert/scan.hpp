// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/convert.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::convert {

inline constexpr std::size_t maximum_conversion_parallelism = 16U;

struct ConversionScanItem {
    // Caller-owned identity carried through unchanged.
    std::size_t item_index{0U};
    std::string source_raw_path;
    formats::AudioSourceSelection selection;
    // Cue-segment bounds; empty converts the whole source.
    std::optional<formats::SampleRange> range;
    std::string destination_raw_path;
    // Carried into the output and verified exactly (see convert_audio_file).
    metadata::MetadataDocument metadata;

    friend bool operator==(const ConversionScanItem&, const ConversionScanItem&) = default;
};

struct ConversionScanOptions {
    EncoderPreset preset;
    std::size_t maximum_parallelism{2U};
    // One resampling policy for the whole scan; absent keeps each source's
    // rate (see AudioConversionRequest::target_sample_rate).
    std::optional<int> target_sample_rate;
    // Downsample-only cap applied per item (ADR-0134); mutually exclusive
    // with target_sample_rate.
    std::optional<int> sample_rate_cap;
    // One stored-bit-depth policy for the whole scan (16 or 24; see
    // AudioConversionRequest::target_bit_depth).
    std::optional<int> target_bit_depth;
    // Keeps each source's stored depth (ADR-0134); mutually exclusive with
    // target_bit_depth.
    bool keep_source_bit_depth{false};
    ConversionChannelPolicy channel_policy{ConversionChannelPolicy::keep};
    ConversionGainMode gain_mode{ConversionGainMode::none};
    float gain_preamp_db{0.0F};
    // Resolves each item's cover image (ADR-0131) and embeds it into the
    // output; a source without usable artwork converts without one.
    bool carry_artwork{false};
};

enum class ConversionScanState : std::uint8_t { pending, running, converted, failed, cancelled };

struct ConvertedItemScan {
    std::size_t item_index{0U};
    std::string source_raw_path;
    std::string destination_raw_path;
    ConversionScanState state{ConversionScanState::pending};
    std::optional<ConvertedAudioFile> converted;
    // Observed before decoding and re-verified afterwards; a source that
    // changed mid-conversion fails the item and its output is removed.
    std::optional<core::LocalSourceRevision> source_revision;
    std::optional<core::Error> issue;
};

struct ConversionScanProgress {
    std::size_t item_index{0U};
    std::string source_raw_path;
    ConversionScanState state{ConversionScanState::pending};
    std::size_t completed_items{0U};
    std::size_t total_items{0U};
    // Within-item decode progress, throttled to roughly one report per
    // second of source audio.
    std::uint64_t frames_done{0U};
    std::optional<std::uint64_t> frames_total;
};

using ConversionScanProgressCallback = std::function<void(const ConversionScanProgress&)>;

struct ConversionScanResult {
    // Aligned with the input items.
    std::vector<ConvertedItemScan> items;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t converted_count() const noexcept;
    [[nodiscard]] std::size_t failed_count() const noexcept;
};

// Converts every item on a bounded worker pool (1-16 workers, one
// decode/encode pipeline per worker), isolating per-item failures.
// Destinations colliding inside one scan resolve atomically: the first
// publication wins, later ones fail as typed conflicts. Cancellation
// stops cleanly after the items already in flight; no partial output
// ever survives.
[[nodiscard]] core::Result<ConversionScanResult>
scan_conversion(std::span<const ConversionScanItem> items, const ConversionScanOptions& options,
                const ConversionScanProgressCallback& progress = {},
                const core::CancellationToken& cancellation = {});

} // namespace trackknife::convert
