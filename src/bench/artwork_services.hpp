// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/operations/artwork_apply.hpp"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// Shared between the UI thread and the background Apply worker; the worker
// writes under the mutex and the inline progress readout copies under it.
struct ArtworkApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::ArtworkApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

inline constexpr std::size_t metadata_artwork_source_limit = 64U;

struct MetadataArtworkScopeSource {
    std::string raw_path;
    std::optional<core::LocalSourceRevision> captured_revision;
    QString label;
    std::vector<std::size_t> occurrence_indexes;
    std::size_t occurrence_count{1U};
    bool captured_revision_consistent{true};

    friend bool operator==(const MetadataArtworkScopeSource&,
                           const MetadataArtworkScopeSource&) = default;
};

using ArtworkWritePlanApplier = std::function<core::Result<operations::ArtworkApplyResult>(
    const metadata::ArtworkWritePlan&, const operations::ArtworkApplyProgressCallback&,
    const core::CancellationToken&)>;
using ArtworkWritePlanApplierFactory = std::function<ArtworkWritePlanApplier()>;
using ArtworkApplyObserver = std::function<void(const operations::ArtworkApplyResult&)>;

// Bench-injected Cover Art Archive boundary (ADR-0091/0094): the listing
// for one release, paced cached byte downloads, and local storage of
// verified PNG/JPEG bytes ready for review. All-empty means cover fetching
// is unavailable.
struct ArtworkCoverArtService {
    std::function<void(const QString& release_id,
                       std::function<void(core::Result<musicbrainz::CoverArtListing>)>)>
        fetch_listing;
    std::function<void(const QString& url, std::function<void(core::Result<QByteArray>)>)>
        fetch_bytes;
    std::function<core::Result<QString>(const QString& identity, const QByteArray& bytes)>
        store_image;
};

} // namespace trackknife::bench
