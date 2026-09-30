// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace trackknife::bench {

// ADR-0237: what the file tools read, measure and probe with -- this
// process, or the engine that holds the files. The tools keep their own code
// and their own screens; only these change. Empty members mean this process.
using LoudnessScanner = std::function<core::Result<loudness::LoudnessScanResult>(
    std::span<const loudness::LoudnessScanItem>, const loudness::LoudnessScanOptions&,
    const loudness::LoudnessScanProgressCallback&, const core::CancellationToken&)>;
using TechnicalsProbe = std::function<core::Result<engine::FileTechnicals>(
    const std::string&, const core::CancellationToken&)>;

using ArtworkStager =
    std::function<core::Result<metadata::ArtworkImageFile>(std::span<const unsigned char>)>;
// Stage 5: how a path plan is checked against the filesystem its files are
// on.
using PathPreflight = std::function<core::Result<operations::OutputPathPreflight>(
    const operations::OutputPathPlan&, const core::CancellationToken&)>;

struct FileWorkTools {
    metadata::MetadataFileAccess access{metadata::local_metadata_file_access()};
    LoudnessScanner scanner{};
    TechnicalsProbe probe{};
    // Stage 4: where artwork is read, and how an image of this computer is
    // handed to the engine to be written from (empty: this process writes).
    operations::ArtworkFileAccess artwork{operations::local_artwork_file_access()};
    ArtworkStager stage{};
    PathPreflight preflight{};
};

// The cover resizer for these tools: drafts here, or resized here from the
// engine's copy and handed back to it.
[[nodiscard]] operations::ArtworkImageFitter artworkFitterFor(const FileWorkTools& tools);

// Images of this computer that intents name -- a cover picked on its disk or
// downloaded -- handed to the engine, so the plan names the engine's copy.
// This process's tools change nothing.
[[nodiscard]] core::Result<std::vector<metadata::ArtworkWritePlanIntent>>
stageReplacements(std::vector<metadata::ArtworkWritePlanIntent> intents, const FileWorkTools& tools,
                  const core::CancellationToken& cancellation);

// The engine's, through its file-work connection.
[[nodiscard]] FileWorkTools engineFileWorkTools(std::shared_ptr<engine::RemoteFileWork> work);

// The tagger's MusicBrainz, Cover Art Archive and AcoustID lookups, made by
// the engine: each off this thread, answered on `context`'s, as the lookups
// made here are.
[[nodiscard]] MusicBrainzLookupService
engineLookupService(std::shared_ptr<engine::RemoteFileWork> work, QObject* context);

} // namespace trackknife::bench
