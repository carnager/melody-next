// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/cue_replay_gain_apply.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace trackknife::operations {

struct LoudnessSidecarAppliedEntry {
    metadata::StagedLogicalIdentity identity;
    std::vector<std::size_t> occurrence_indexes;
    std::vector<CueReplayGainAppliedField> fields;

    friend bool operator==(const LoudnessSidecarAppliedEntry&,
                           const LoudnessSidecarAppliedEntry&) = default;
};

struct LoudnessSidecarCommitResult {
    std::string raw_audio_path;
    std::string sidecar_raw_path;
    core::LocalSourceRevision audio_revision;
    // True when the merge left no entries and the sidecar file was
    // removed instead of rewritten.
    bool sidecar_removed{false};
    std::vector<LoudnessSidecarAppliedEntry> entries;
    // The write's journal entry, whose backup is released by it.
    core::StableId journal_id;

    friend bool operator==(const LoudnessSidecarCommitResult&,
                           const LoudnessSidecarCommitResult&) = default;
};

// Executes one ready loudness-sidecar plan (ADR-0141): revision-gated
// on the audio file's capture evidence, read-merge-write against the
// existing sidecar (an unparseable sidecar is a conflict, never
// clobbered; stale entries are dropped wholesale). Merges over an
// existing sidecar run the full ADR-0059 journal lifecycle (ADR-0145),
// publishing an empty-entries document instead of deleting when the
// merge empties it; creation of a missing sidecar stays a direct
// atomic publish because there is no pre-image to protect. Runs on a
// bounded mutation worker.
[[nodiscard]] core::Result<LoudnessSidecarCommitResult>
commit_loudness_sidecar(const metadata::MetadataWritePlanSidecar& sidecar_plan,
                        MetadataOperationJournal& journal,
                        const MetadataDependentStateCommitter& dependent_state_committer,
                        const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
