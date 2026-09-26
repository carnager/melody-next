// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::core {

struct LocalSourceIssue {
    std::string raw_path;
    Error error;

    friend bool operator==(const LocalSourceIssue&, const LocalSourceIssue&) = default;
};

struct LocalSourceDiscovery {
    std::vector<std::string> raw_files;
    std::vector<LocalSourceIssue> issues;
    std::size_t visited_entries{0U};
    bool cancelled{false};
    bool truncated{false};
};

// Expands regular files and directories without interpreting raw Linux path
// bytes as UTF-8. Directory traversal is recursive, deterministic, and does
// not follow directory symlinks. Input order and duplicate occurrences are
// preserved. When directory_file_extensions is non-empty, files found by
// directory expansion must match one of the extensions (ASCII
// case-insensitive, given without the dot); explicitly listed files always
// pass so a user-chosen file is never silently dropped.
[[nodiscard]] LocalSourceDiscovery
discover_local_sources(std::span<const std::string> raw_paths,
                       const CancellationToken& cancellation, std::size_t file_limit = 100'000U,
                       std::span<const std::string_view> directory_file_extensions = {});

// Produces valid UTF-8 ASCII for presentation while retaining every raw byte.
[[nodiscard]] std::string escape_raw_path(std::string_view raw_path);
// A raw path for people to read: valid UTF-8 stays as written ("Kopfhörer",
// not "Kopfh\xC3\xB6rer"); only bytes that are not UTF-8, control
// characters, line separators and invisible format marks are escaped as
// escape_raw_path would, so nothing in a name can hide or disguise itself.
[[nodiscard]] std::string display_raw_path(std::string_view raw_path);

struct ContainedLocalSource {
    std::string raw_root;
    std::string raw_path;
    // The fully resolved final target after following every symlink.
    std::string resolved_path;

    friend bool operator==(const ContainedLocalSource&, const ContainedLocalSource&) = default;
};

// A Linux filesystem observation suitable for detecting replacement or
// modification between metadata read, preview, and commit. It is evidence,
// not a permanent identity: callers must observe it again immediately before
// mutation.
struct LocalSourceRevision {
    std::uint64_t device{0U};
    std::uint64_t inode{0U};
    std::uint64_t size{0U};
    std::int64_t modification_time_seconds{0};
    std::int64_t modification_time_nanoseconds{0};

    friend bool operator==(const LocalSourceRevision&, const LocalSourceRevision&) = default;
};

// Follows the final path target and observes a regular file without decoding
// the raw Linux path bytes as text.
[[nodiscard]] Result<LocalSourceRevision>
observe_local_source_revision(const std::string& raw_path);

// Whether a file is one the library indexes, by its extension (ASCII, any
// case). One list, for the index and for anything that tells it about files
// (ADR-0232), so the two cannot disagree about what counts.
[[nodiscard]] bool is_audio_path(std::string_view raw_path);

// Filesystem revalidation for file operations: resolves the configured root
// and the referenced path to their final targets and requires the result to
// be a regular file strictly inside the resolved root. This is the
// symlink/mount complement to the lexical mapping guards; callers revalidate
// immediately before offering or executing a file operation, and a stale
// success never survives a later filesystem change because every operation
// revalidates again.
[[nodiscard]] Result<ContainedLocalSource> revalidate_contained_source(const std::string& raw_root,
                                                                       const std::string& raw_path);

} // namespace trackknife::core
