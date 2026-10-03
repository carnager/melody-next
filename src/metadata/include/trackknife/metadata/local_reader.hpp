// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <string>

namespace trackknife::metadata {

struct MetadataCapabilities {
    bool fields_readable{false};
    bool fields_writable{false};
    bool pictures_readable{false};
    bool pictures_writable{false};
    bool unknown_data_preserved_on_write{false};

    friend bool operator==(const MetadataCapabilities&, const MetadataCapabilities&) = default;
};

struct LocalMetadataRead {
    std::string raw_path;
    core::LocalSourceRevision source_revision;
    MetadataDocument document;
    std::string adapter_name;
    MetadataCapabilities capabilities;
    // ADR-0237 stage 2: an MP3's popularimeter (POPM) rating byte -- the
    // Windows Media Player owner's, else the first -- which is no text tag.
    std::optional<std::uint8_t> popularimeter;

    friend bool operator==(const LocalMetadataRead&, const LocalMetadataRead&) = default;
};

// Reads TagLib's generic text property projection at one observed source
// revision. The synchronous backend call is bracketed by cancellation and
// revision checks; callers run it on a bounded worker, never the UI thread.
[[nodiscard]] core::Result<LocalMetadataRead>
read_local_metadata(const std::string& raw_path, const core::CancellationToken& cancellation = {});

// Where files are read from: this process, or -- ADR-0237 -- the engine that
// holds them. `read` is read_local_metadata; `revision` observes a file whose
// format has no tags to read, which may still take gains in a sidecar.
struct MetadataFileAccess {
    std::function<core::Result<LocalMetadataRead>(const std::string&,
                                                  const core::CancellationToken&)>
        read;
    std::function<core::Result<core::LocalSourceRevision>(const std::string&)> revision;
    // Many files in one go, answered in order, one result each: an engine
    // elsewhere reads a batch in one request rather than one request a file.
    // Its own error is the batch not asked at all -- the engine gone -- as
    // against a file it could not read. Empty, `read` is asked file by file.
    std::function<core::Result<std::vector<core::Result<LocalMetadataRead>>>(
        const std::vector<std::string>&, const core::CancellationToken&)>
        read_many;
};

// What capture_metadata_sources made of a selection: every source, in order,
// and the files that could not be read, with why -- one unreadable file among
// thousands is no reason to show none. Such a file stays in with what was
// cached and no source revision, which no write takes as its baseline.
struct CapturedMetadataSources {
    std::vector<StagedMetadataSource> sources;
    std::vector<std::pair<std::string, core::Error>> unreadable;
};

// Told how many of the files to read have been, of how many.
using MetadataCaptureProgress = std::function<void(std::size_t read, std::size_t total)>;

// As many files as one read_many is given.
inline constexpr std::size_t metadata_capture_batch = 256U;

// This process's own file access.
[[nodiscard]] MetadataFileAccess local_metadata_file_access();

// Prepare revisionless physical cache rows for an explicit metadata operation.
// Repeated paths share one fresh read; existing revisions and logical overlays
// remain subject to the existing stale-source checks. Run on a worker.
[[nodiscard]] core::Result<std::vector<StagedMetadataSource>>
capture_uncached_metadata_sources(std::vector<StagedMetadataSource> sources,
                                  const core::CancellationToken& cancellation = {});
[[nodiscard]] core::Result<std::vector<StagedMetadataSource>>
capture_uncached_metadata_sources(std::vector<StagedMetadataSource> sources,
                                  const MetadataFileAccess& access,
                                  const core::CancellationToken& cancellation = {});
// The same, for a selection of any size: files read in batches (read_many
// where the access has it), progress told after each, and a file that cannot
// be read kept unread rather than failing the rest. Fails only when cancelled,
// or when the engine cannot be reached at all. Run on a worker.
[[nodiscard]] core::Result<CapturedMetadataSources>
capture_metadata_sources(std::vector<StagedMetadataSource> sources,
                         const MetadataFileAccess& access,
                         const core::CancellationToken& cancellation = {},
                         const MetadataCaptureProgress& progress = {});

} // namespace trackknife::metadata
