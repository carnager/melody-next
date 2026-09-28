// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/loudness_sidecar.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::loudness {

// ADR-0156: the ReplayGain measurement-to-proposal pipeline -- grouping, R128
// (ADR-0149), peak policy (ADR-0148), problems and retries (ADR-0146), the
// measurement snapshot (ADR-0147) -- in one place for every client of it:
// Trackknife's ReplayGain windows and the engine's replaygain.apply job
// (ADR-0237), so a gain is the same whoever asked for it.

struct ReplayGainSettings {
    LoudnessGrouping grouping;
    bool true_peak{false};
    bool sidecar_only{false};
};

// Where in a file an item's audio is: the whole file, or one stream, subsong
// or CUE range of it.
struct ReplayGainAudio {
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> range;
};

struct ReplayGainProblem {
    // The file it is about; none for an album programme as a whole.
    std::optional<std::string> raw_path;
    std::string message;
};

// One measured item as the snapshot records it; values are text, empty where
// nothing was measured.
struct ReplayGainRow {
    std::string title;
    std::string raw_path;
    std::string integrated_lufs;
    std::string track_gain;
    std::string track_peak;
    std::string album_key;
    std::string album_gain;
    std::string album_peak;
    std::string status;
    std::string peak_kind;
};

struct ReplayGainMeasurement {
    metadata::MetadataProposalSet proposals;
    std::vector<ReplayGainProblem> problems;
    // Items whose measurement failed or was cancelled and can be re-run;
    // structurally unmeasurable tracks are not among them.
    std::vector<std::size_t> retry_items;
    std::vector<ReplayGainRow> rows;
};

using ReplayGainScanner = std::function<core::Result<LoudnessScanResult>(
    std::span<const LoudnessScanItem>, const LoudnessScanOptions&,
    const LoudnessScanProgressCallback&, const core::CancellationToken&)>;

// Measures `items` of `selection` (tags as `draft` would leave them decide the
// grouping) and proposes their ReplayGain fields. `scanner` empty: this
// process scans.
[[nodiscard]] core::Result<ReplayGainMeasurement>
measure_replaygain(const metadata::StagedMetadataSelection& selection,
                   const metadata::StagedMetadataPatchSet& draft,
                   const std::vector<std::size_t>& items, std::span<const ReplayGainAudio> audio,
                   const ReplayGainSettings& settings, const LoudnessScanProgressCallback& progress,
                   const core::CancellationToken& cancellation,
                   const ReplayGainScanner& scanner = {});

// ADR-0141: the gains a loudness sidecar holds for one item -- a whole file,
// or its stream, subsong or CUE range -- added to its document at sidecar
// provenance, the highest, as they would read back. The caller has checked
// the sidecar is fresh for the file's revision.
void project_loudness_sidecar(metadata::MetadataDocument& document,
                              const metadata::LoudnessSidecar& sidecar,
                              const formats::AudioSourceSelection& selection,
                              const std::optional<formats::SampleRange>& range);

// The proposals as ordinary staged patches, on a copy of the selection whose
// vocabulary grows the loudness fields.
struct StagedReplayGain {
    metadata::StagedMetadataSelection selection;
    metadata::StagedMetadataPatchSet patches;
    std::size_t staged_fields{0U};
};
[[nodiscard]] core::Result<StagedReplayGain>
stage_replaygain(const metadata::StagedMetadataSelection& selection,
                 const metadata::MetadataProposalSet& proposals,
                 const core::CancellationToken& cancellation);

} // namespace trackknife::loudness
