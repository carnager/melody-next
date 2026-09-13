// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/flac_writer.hpp"

#include <string>

namespace trackknife::metadata {

using PreparedArtworkWrite = PreparedFlacArtworkWrite;

// Prepared-copy artwork writers for MP3 (ID3v2 APIC frames) and MP4 (covr
// entries), mirroring the FLAC driver (ADR-0137): revision brackets, fresh
// inventory revalidation, exclusive copy, the change applied through
// TagLib, then verification — text document unchanged, resulting inventory
// exactly as planned with every unrelated picture byte-identical by
// fingerprint, and the container preservation proof (MPEG audio region
// plus APEv2 items; MP4 ftyp/mdat/other boxes). The APIC writer mutates
// the targeted frame in place, keeping ordinals, types, and descriptions;
// the covr writer produces untyped entries and rejects a non-empty added
// description as unsupported. Synchronous I/O — bounded mutation workers
// only; the user's file is never replaced here.
[[nodiscard]] core::Result<PreparedArtworkWrite>
prepare_mp3_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation = {});

[[nodiscard]] core::Result<PreparedArtworkWrite>
prepare_mp4_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation = {});

[[nodiscard]] core::Result<PreparedFlacMetadataWrite>
prepare_composed_metadata_write_copy(const MetadataWritePlanSource&, const std::string&,
                                     const core::CancellationToken& = {});

// Dispatches one ready artwork write-plan source to its qualified writer
// by adapter name, mirroring prepare_qualified_metadata_write_copy.
[[nodiscard]] core::Result<PreparedArtworkWrite>
prepare_qualified_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                                     const std::string& prepared_raw_path,
                                     const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
