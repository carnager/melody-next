// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/preparation_feedback_dialog.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <QStringList>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace trackknife::bench {

struct MetadataPropertiesAudioSource {
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> range;
};

// ADR-0156: the ReplayGain measurement-to-proposal pipeline shared by
// the Properties workspace and the compact context-menu dialog — one
// implementation for grouping, R128 (ADR-0149), peak policy (ADR-0148),
// problems, retries, and the CSV snapshot.
struct ReplayGainScanOutcome {
    core::Result<metadata::MetadataProposalSet> proposals{metadata::MetadataProposalSet{}};
    std::vector<PreparationFeedbackRow> problems;
    // ADR-0146: items whose measurement failed or was cancelled and can
    // be re-run; structurally unmeasurable tracks are excluded.
    std::vector<std::size_t> retry_items;
    // ADR-0147: pre-escaped CSV data rows snapshotting the measurement.
    QStringList export_rows;
};

struct ReplayGainScanSettings {
    loudness::LoudnessGrouping grouping;
    bool true_peak{false};
    bool sidecar_only{false};
};

[[nodiscard]] std::shared_ptr<ReplayGainScanOutcome> run_replaygain_scan(
    std::shared_ptr<const metadata::StagedMetadataSelection> selection,
    const metadata::StagedMetadataPatchSet& draft, const std::vector<std::size_t>& items,
    const std::shared_ptr<const std::vector<MetadataPropertiesAudioSource>>& audio_sources,
    const ReplayGainScanSettings& settings, const std::shared_ptr<std::atomic_size_t>& completed,
    const core::CancellationToken& cancellation);

} // namespace trackknife::bench
