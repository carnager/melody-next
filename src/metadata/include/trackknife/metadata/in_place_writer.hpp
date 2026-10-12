// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::metadata {

// ADR-0281: one stretch of the file a write in place replaces.
struct InPlaceRange {
    std::uint64_t offset{0U};
    std::string before;
    std::string after;

    friend bool operator==(const InPlaceRange&, const InPlaceRange&) = default;
};

struct InPlaceMetadataWrite {
    std::vector<InPlaceRange> ranges;
    // The tags the file holds once the ranges are written.
    MetadataDocument document;
    std::optional<std::uint8_t> popularimeter;
    std::uint64_t length{0U};
};

// The tags a plan writes, written by TagLib as a prepared copy would be but
// into an overlay over the file, which is only read. Answers the ranges to
// write -- checked to lie outside the audio and every other preserved part
// of the file, and the tags read back from them checked against the plan --
// or nothing when the write does not fit in the file as it is (the tags
// outgrow their padding) or the format is not written in place: then the
// file takes the prepared-copy path.
[[nodiscard]] core::Result<std::optional<InPlaceMetadataWrite>>
prepare_in_place_metadata_write(const MetadataWritePlanSource& source_plan,
                                const core::CancellationToken& cancellation = {});

} // namespace trackknife::metadata
