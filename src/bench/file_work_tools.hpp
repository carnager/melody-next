// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_identify_dialog.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace trackknife::bench {

// ADR-0237: what the file tools read, measure and probe with -- this
// process, or the engine that holds the files. The tools keep their own code
// and their own screens; only these change. Empty members mean this process.
using LoudnessScanner = std::function<core::Result<loudness::LoudnessScanResult>(
    std::span<const loudness::LoudnessScanItem>, const loudness::LoudnessScanOptions&,
    const loudness::LoudnessScanProgressCallback&, const core::CancellationToken&)>;
using TechnicalsProbe = std::function<core::Result<engine::FileTechnicals>(
    const std::string&, const core::CancellationToken&)>;

struct FileWorkTools {
    metadata::MetadataFileAccess access{metadata::local_metadata_file_access()};
    LoudnessScanner scanner{};
    TechnicalsProbe probe{};
};

// The engine's, through its file-work connection.
[[nodiscard]] FileWorkTools engineFileWorkTools(std::shared_ptr<engine::RemoteFileWork> work);

// The tagger's MusicBrainz, Cover Art Archive and AcoustID lookups, made by
// the engine: each off this thread, answered on `context`'s, as the lookups
// made here are.
[[nodiscard]] MusicBrainzLookupService
engineLookupService(std::shared_ptr<engine::RemoteFileWork> work, QObject* context);

} // namespace trackknife::bench
