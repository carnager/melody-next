// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::operations {

enum class OutputPathPublicationKind : std::uint8_t {
    no_change = 0,
    same_filesystem_rename = 1,
    cross_filesystem_copy = 2,
};

enum class OutputPathPreflightIssueKind : std::uint8_t {
    source_missing,
    source_symlink,
    source_not_regular,
    source_changed,
    source_hard_linked,
    source_parent_not_writable,
    operation_root_missing,
    operation_root_symlink,
    operation_root_not_directory,
    target_parent_symlink,
    target_parent_not_directory,
    target_parent_not_writable,
    target_exists,
    component_too_long,
    path_too_long,
    filesystem_observation_failed,
    // ADR-0318: two companions would go to one target.
    companion_target_conflict,
    // ADR-0318: a CUE sheet names an audio file the plan renames.
    cue_reference_broken,
};

[[nodiscard]] std::string_view
output_path_preflight_issue_kind_name(OutputPathPreflightIssueKind kind);

struct OutputPathPreflightIssue {
    OutputPathPreflightIssueKind kind{OutputPathPreflightIssueKind::filesystem_observation_failed};
    bool blocking{true};
    std::string message;
    std::vector<std::size_t> item_indexes;
    std::string source_raw_path;
    std::string target_raw_path;

    friend bool operator==(const OutputPathPreflightIssue&,
                           const OutputPathPreflightIssue&) = default;
};

struct OutputPathPreflightSource {
    PlannedOutputPathSource planned;
    core::LocalSourceRevision observed_revision;
    OutputPathPublicationKind publication{OutputPathPublicationKind::no_change};
    std::uint64_t target_filesystem_device{0U};
    // These directories were absent during preflight. Execution must observe
    // again before creating them; the preflight itself never mutates them.
    std::vector<std::string> missing_directory_raw_paths;

    friend bool operator==(const OutputPathPreflightSource&,
                           const OutputPathPreflightSource&) = default;
};

struct OutputPathPreflight {
    OutputPathPlan plan;
    std::vector<OutputPathPreflightSource> sources;
    std::vector<OutputPathPreflightIssue> issues;

    [[nodiscard]] bool ready() const noexcept;

    friend bool operator==(const OutputPathPreflight&, const OutputPathPreflight&) = default;
};

// Fresh Linux filesystem observation for a ready pure plan. Every path
// component is opened without following symlinks. This classifies publication
// but performs no mkdir, copy, rename, unlink, or metadata mutation.
[[nodiscard]] core::Result<OutputPathPreflight>
preflight_output_paths(const OutputPathPlan& plan,
                       const core::CancellationToken& cancellation = {});

// ADR-0320: a CUE sheet's FILE names, renamed as `renamed` says -- names
// as written, quoted or not -- every other byte kept, line endings with
// them. Nothing where it names none of them.
[[nodiscard]] std::optional<std::string>
rewrite_cue_file_names(std::string_view sheet, const std::map<std::string, std::string>& renamed);

// ADR-0318: the same, with the companions of the files that move: where
// every audio file of a folder goes to one other folder, that folder's
// images, CUE sheets and playlists go with them, planned as further sources
// (`companion`) and checked as every source is. A CUE sheet naming a file the
// plan renames travels rewritten to name it as renamed (ADR-0320).
[[nodiscard]] core::Result<OutputPathPreflight>
preflight_output_paths_with_companions(const OutputPathPlan& plan,
                                       const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
