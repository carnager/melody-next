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
