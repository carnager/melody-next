// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

struct ArtworkExportRequest {
    metadata::ArtworkInventoryItem source;
    std::string destination_raw_path;

    friend bool operator==(const ArtworkExportRequest&, const ArtworkExportRequest&) = default;
};

enum class ArtworkExportItemState : std::uint8_t {
    pending,
    running,
    exported,
    failed,
    cancelled,
};

struct ArtworkExportItemResult {
    std::size_t item_index{0U};
    std::string destination_raw_path;
    ArtworkExportItemState state{ArtworkExportItemState::pending};
    std::optional<core::Error> issue;

    friend bool operator==(const ArtworkExportItemResult&,
                           const ArtworkExportItemResult&) = default;
};

struct ArtworkExportProgress {
    std::size_t item_index{0U};
    ArtworkExportItemState state{ArtworkExportItemState::pending};
    std::size_t completed_items{0U};
    std::size_t total_items{0U};
    std::optional<core::Error> issue;

    friend bool operator==(const ArtworkExportProgress&, const ArtworkExportProgress&) = default;
};

struct ArtworkExportResult {
    std::vector<ArtworkExportItemResult> items;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t exported_item_count() const noexcept;
    [[nodiscard]] std::size_t failed_item_count() const noexcept;
    [[nodiscard]] std::size_t cancelled_item_count() const noexcept;

    friend bool operator==(const ArtworkExportResult&, const ArtworkExportResult&) = default;
};

struct ArtworkExportOptions {
    std::size_t maximum_parallelism{2U};
    std::uint64_t maximum_item_bytes{16U * 1024U * 1024U};
    // Where the images are read: this process when empty, or (ADR-0237) the
    // engine holding the files. The destinations are always this process's.
    std::function<core::Result<std::vector<unsigned char>>(
        const metadata::ArtworkImageFile&, std::uint64_t, const core::CancellationToken&)>
        image_bytes{};
};

using ArtworkExportProgressCallback = std::function<void(const ArtworkExportProgress&)>;

// Rereads each reviewed image under its revision/hash evidence and creates the
// exact destination with O_EXCL. Existing paths are never overwritten. Work
// is bounded and failed/cancelled partial files owned by this call are removed.
[[nodiscard]] core::Result<ArtworkExportResult>
export_artwork_items(const std::vector<ArtworkExportRequest>& requests,
                     const ArtworkExportProgressCallback& progress = {},
                     const core::CancellationToken& cancellation = {},
                     const ArtworkExportOptions& options = {});

} // namespace trackknife::operations
