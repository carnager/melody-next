// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

// One applied REM value in its canonical sheet text, for occurrence
// refresh. An absent value means the REM line was removed.
struct CueReplayGainAppliedField {
    std::string canonical_name;
    std::string display_name;
    std::optional<std::string> value;

    friend bool operator==(const CueReplayGainAppliedField&,
                           const CueReplayGainAppliedField&) = default;
};

struct CueReplayGainAppliedTrack {
    std::size_t file_index{0U};
    std::size_t track_index{0U};
    std::vector<std::size_t> occurrence_indexes;
    std::vector<CueReplayGainAppliedField> fields;

    friend bool operator==(const CueReplayGainAppliedTrack&,
                           const CueReplayGainAppliedTrack&) = default;
};

struct CueReplayGainCommitResult {
    std::string raw_cue_path;
    core::LocalSourceRevision previous_revision;
    core::LocalSourceRevision published_revision;
    // Album fields are a sheet-header property: they project onto every
    // logical track of the sheet, not only the planned ones.
    std::vector<CueReplayGainAppliedField> album_fields;
    std::vector<CueReplayGainAppliedTrack> tracks;
    // The write's journal entry, whose backup is released by it.
    core::StableId journal_id;

    friend bool operator==(const CueReplayGainCommitResult&,
                           const CueReplayGainCommitResult&) = default;
};

// Executes one ready CUE-sheet ReplayGain plan (ADR-0139) through the
// full ADR-0059 journal lifecycle (ADR-0145): revision-gated against
// the draft-capture evidence, rewritten through the proven
// byte-preserving REM rewriter, prepared at the journal's sibling
// path, published with a retained undoable backup, and recovered after
// crashes like every other journaled mutation. Runs on a bounded
// mutation worker, never the UI thread.
[[nodiscard]] core::Result<CueReplayGainCommitResult>
commit_cue_replay_gain_sheet(const metadata::MetadataWritePlanCueSheet& sheet_plan,
                             MetadataOperationJournal& journal,
                             const MetadataDependentStateCommitter& dependent_state_committer,
                             const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
