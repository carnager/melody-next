// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/operations/metadata_commit.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

// Converts an image to one whose longer edge is at most max_edge, written as a
// file the plan can reference, and inspected like any replacement input. It
// may return the input when it already fits. Decoding lives with the caller,
// so this layer stays free of an image library.
using ArtworkImageFitter = std::function<core::Result<metadata::ArtworkImageFile>(
    const metadata::ArtworkImageFile& image, std::uint32_t max_edge,
    const core::CancellationToken& cancellation)>;

// A cover over a size limit may be larger than a replacement input is allowed
// to be: shrinking it is the point.
inline constexpr std::uint64_t maximum_fittable_artwork_bytes = 64U * 1024U * 1024U;

// Captures policy destinations and detects conflicting images for a shared
// folder. A policy with a size limit needs a fitter.
[[nodiscard]] core::Result<metadata::ArtworkWritePlan>
plan_artwork_storage(const std::vector<metadata::ArtworkWritePlanIntent>& intents,
                     const metadata::ArtworkStoragePolicy& policy,
                     const core::CancellationToken& cancellation = {},
                     const ArtworkImageFitter& fitter = {});

enum class ArtworkApplySourceState : std::uint8_t {
    pending,
    running,
    committed,
    failed,
    cancelled,
};

struct ArtworkApplySourceResult {
    std::size_t source_index{0U};
    std::string raw_path;
    ArtworkApplySourceState state{ArtworkApplySourceState::pending};
    std::optional<MetadataCommitResult> commit;
    std::optional<core::Error> issue;

    friend bool operator==(const ArtworkApplySourceResult&,
                           const ArtworkApplySourceResult&) = default;
};

struct ArtworkApplyProgress {
    std::size_t source_index{0U};
    std::string raw_path;
    ArtworkApplySourceState state{ArtworkApplySourceState::pending};
    std::size_t completed_sources{0U};
    std::size_t total_sources{0U};
    std::optional<core::Error> issue;

    friend bool operator==(const ArtworkApplyProgress&, const ArtworkApplyProgress&) = default;
};

struct ArtworkApplyResult {
    std::vector<ArtworkApplySourceResult> sources;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t committed_source_count() const noexcept;
    [[nodiscard]] std::size_t failed_source_count() const noexcept;
    [[nodiscard]] std::size_t cancelled_source_count() const noexcept;

    friend bool operator==(const ArtworkApplyResult&, const ArtworkApplyResult&) = default;
};

struct ArtworkApplyOptions {
    std::size_t maximum_parallelism{2U};
};

using ArtworkApplySourceCommitter = std::function<core::Result<MetadataCommitResult>(
    const metadata::ArtworkWritePlanSource&, const core::CancellationToken&)>;
using ArtworkApplyProgressCallback = std::function<void(const ArtworkApplyProgress&)>;

// Applies one entirely ready immutable artwork plan on a bounded worker pool.
// Steps in one physical file execute serially, advancing revisions only through
// successful journaled commits; distinct files execute in parallel. Failure stops
// remaining steps in that file; earlier steps remain committed. Results and
// progress count changes (a file can occur more than once). Cancellation stops
// admission of new work.
[[nodiscard]] core::Result<ArtworkApplyResult> apply_artwork_write_plan(
    const metadata::ArtworkWritePlan& plan, const ArtworkApplySourceCommitter& committer,
    const ArtworkApplyProgressCallback& progress = {},
    const core::CancellationToken& cancellation = {}, const ArtworkApplyOptions& options = {});

} // namespace trackknife::operations
