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

// ADR-0331: whether the filesystem an open file is on numbers its files
// itself, on this side -- SMB/CIFS (mounted noserverino, or a server that
// gives no inode numbers) and FUSE (sshfs) -- so that a file's inode number
// can change while the file does not: forgotten by the kernel and looked up
// again, it is given another.
[[nodiscard]] bool inode_numbers_unstable(int descriptor) noexcept;

// The open file `descriptor` as `observed` is still the one `expected`
// describes: the same revision -- or, where inode numbers are unstable,
// the same but for the inode, its device, size and modification time to the
// nanosecond as they were.
[[nodiscard]] bool same_revision(const LocalSourceRevision& expected,
                                 const LocalSourceRevision& observed, int descriptor) noexcept;
// The rule itself, told whether inode numbers are unstable.
[[nodiscard]] bool same_revision(const LocalSourceRevision& expected,
                                 const LocalSourceRevision& observed,
                                 bool inode_numbers_unstable) noexcept;

// A file Trackknife has just written given a modification time of its own:
// now, rounded down to an even second, which FAT's two-second steps and SMB's
// 100 ns units both store exactly. On a network mount the client caches a
// fresh file's attributes -- the macOS SMB client reported a just-written
// file's time as its own guess, then the server's, then its guess again, for
// about two seconds -- and a time set explicitly, after the writing is done,
// is the one both then hold. Only for files written here, never a user's.
// Best effort: where a filesystem refuses to set times -- some sshfs and
// FUSE mounts do -- the file keeps the time it has, as before.
void settle_written_file_time(const std::string& raw_path) noexcept;
// The same for a file written in place, where size and inode stay as they
// were: the time set is always later than `before`'s, so the write -- and
// one made in the two seconds after it -- is still seen as a change.
void settle_rewritten_file_time(const std::string& raw_path,
                                const LocalSourceRevision& before) noexcept;

// ADR-0249, ADR-0343: whether `observed`, the file at `raw_path` now, is the
// one `expected` describes -- the rule every check of a file seen earlier
// keeps to. Equal; or equal in all but the inode where the file's number may
// have changed under it: on a filesystem that numbers its files on this
// side (inode_numbers_unstable: SMB/CIFS, FUSE such as sshfs), where a file
// the kernel forgets is numbered afresh, or on one whose renames give a file
// a new number, as the macOS SMB client does. Whether renames keep numbers
// is asked of the filesystem itself, once per device and only when an inode
// did change: a temporary file beside `raw_path` is renamed and observed.
// Where neither can be told, an inode change counts.
[[nodiscard]] bool same_file(const std::string& raw_path, const LocalSourceRevision& expected,
                             const LocalSourceRevision& observed);

// Tests only: every filesystem answers that renames renumber files.
void simulate_renumbering_renames_for_testing(bool enabled) noexcept;
// Tests only: every filesystem answers that it numbers its files itself.
void simulate_unstable_inode_numbers_for_testing(bool enabled) noexcept;

// What differs between two observations of one file, for the message that
// refuses it: "modification time 1790815120.409922300 -> 1790815121.0", each
// field that changed, joined by "; ". Empty when none did.
[[nodiscard]] std::string describe_revision_change(const LocalSourceRevision& before,
                                                   const LocalSourceRevision& after);

// Follows the final path target and observes a regular file without decoding
// the raw Linux path bytes as text.
[[nodiscard]] Result<LocalSourceRevision>
observe_local_source_revision(const std::string& raw_path);

// Whether a file is one the library indexes, by its extension (ASCII, any
// case). One list, for the index and for anything that tells it about files
// (ADR-0232), so the two cannot disagree about what counts. Never a hidden
// file, its name starting with a dot (ADR-0308): a sync tool's fragment --
// ChronoSync's ".CS080.Song.m4a" -- or macOS's "._Song.m4a" beside a track.
[[nodiscard]] bool is_audio_path(std::string_view raw_path);

// Whether a file or folder under a library root is in a hidden folder, or is
// hidden itself: a name below `root_raw_path` starting with a dot. The root
// itself may be in one -- ~/.local/share/music -- and still be indexed.
[[nodiscard]] bool hidden_below_root(std::string_view raw_path, std::string_view root_raw_path);

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
