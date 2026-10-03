// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/file_work_tools.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/operations/preparation_plan.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// What one Apply writes, as the tagger and Identify albums… ask it: the
// draft over the files read, which of them, and what is done to them.
struct PreparationRequest {
    std::shared_ptr<const metadata::StagedMetadataSelection> selection;
    // WYSIWYG: exactly what is staged. Empty when tags are not saved.
    metadata::StagedMetadataPatchSet draft;
    // The rows renamed or moved.
    std::vector<std::size_t> items;
    operations::PreparationOperationSelection operations;
    std::optional<operations::OutputLayoutProfile> layout;
    std::optional<operations::DestinationProfile> destination;
    metadata::MetadataWritePlanOptions options;
    std::vector<metadata::ArtworkWritePlanIntent> artwork;
    metadata::ArtworkStoragePolicy cover_policy;
    FileWorkTools tools;
};

// The plan, every file checked against what it is now. Run on a worker.
[[nodiscard]] core::Result<operations::PreparationPlan>
planPreparation(PreparationRequest request, const core::CancellationToken& cancellation);

// The library folder `raw_path` is in: the deepest of `roots` holding it.
[[nodiscard]] std::optional<std::string> libraryFolderOf(const std::string& raw_path,
                                                         const std::vector<std::string>& roots);

// A folder moved into, as a destination.
[[nodiscard]] operations::DestinationProfile folderDestination(std::string root_raw_path);

// How a plan treats loudness, as Settings say.
[[nodiscard]] metadata::MetadataWritePlanOptions writePlanOptions();

// `draft` without the rows not in `items`.
[[nodiscard]] core::Result<metadata::StagedMetadataPatchSet>
draftOf(const metadata::StagedMetadataSelection& selection,
        const metadata::StagedMetadataPatchSet& draft, std::span<const std::size_t> items);

} // namespace trackknife::bench
