// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/protocol/message.hpp"

namespace trackknife::engine::wire {

// ADR-0237: file work runs in the engine, and what it reads, measures and
// writes crosses protocol v1 unchanged. Each encoding here is exact -- a value
// decoded is equal to the value encoded -- because the preview a client shows
// is the contract the engine later holds a write to. Raw paths travel as
// encoded bytes, never as text; a double that is not finite (a track too
// short to have a loudness) travels as the string "inf", "-inf" or "nan".
//
// Decoding refuses a malformed document with invalid_argument rather than
// guessing: a client and an engine that disagree about a plan must not write.

using protocol::Json;

[[nodiscard]] Json encode(const core::Error& error);
[[nodiscard]] core::Result<core::Error> decode_error(const Json& value);

[[nodiscard]] Json encode(const core::LocalSourceRevision& revision);
[[nodiscard]] core::Result<core::LocalSourceRevision> decode_revision(const Json& value);

[[nodiscard]] Json encode(const formats::AudioSourceSelection& selection);
[[nodiscard]] core::Result<formats::AudioSourceSelection> decode_selection(const Json& value);

[[nodiscard]] Json encode(const formats::SampleRange& range);
[[nodiscard]] core::Result<formats::SampleRange> decode_range(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanItem& item);
[[nodiscard]] core::Result<loudness::LoudnessScanItem> decode_scan_item(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanOptions& options);
[[nodiscard]] core::Result<loudness::LoudnessScanOptions> decode_scan_options(const Json& value);

[[nodiscard]] Json encode(const loudness::LoudnessScanResult& result);
[[nodiscard]] core::Result<loudness::LoudnessScanResult> decode_scan_result(const Json& value);

// Tag text is valid UTF-8 almost always, and travels as a JSON string then;
// anything else travels as {"bytes": encoded}, so a value is never altered.
[[nodiscard]] Json encode_text(std::string_view text);
[[nodiscard]] core::Result<std::string> decode_text(const Json& value);

[[nodiscard]] Json encode(const metadata::MetadataDocument& document);
[[nodiscard]] core::Result<metadata::MetadataDocument> decode_document(const Json& value);

[[nodiscard]] Json encode(const metadata::MetadataCapabilities& capabilities);
[[nodiscard]] core::Result<metadata::MetadataCapabilities> decode_capabilities(const Json& value);

[[nodiscard]] Json encode(const metadata::LocalMetadataRead& read);
[[nodiscard]] core::Result<metadata::LocalMetadataRead> decode_metadata_read(const Json& value);

// The plan a client previewed and the engine writes. A plan carrying artwork
// is refused on decoding: artwork is written by the engine from stage 4.
[[nodiscard]] Json encode(const metadata::MetadataWritePlan& plan);
[[nodiscard]] core::Result<metadata::MetadataWritePlan> decode_write_plan(const Json& value);

[[nodiscard]] Json encode(const operations::MetadataApplyResult& result);
[[nodiscard]] core::Result<operations::MetadataApplyResult> decode_apply_result(const Json& value);

[[nodiscard]] Json encode(const operations::MetadataApplyProgress& progress);
[[nodiscard]] core::Result<operations::MetadataApplyProgress>
decode_apply_progress(const Json& value);

// ADR-0237 stage 4: artwork. Images are named by path, revision and content
// fingerprint, never carried as bytes in these.
[[nodiscard]] Json encode(const metadata::ArtworkImageFile& image);
[[nodiscard]] core::Result<metadata::ArtworkImageFile> decode_image_file(const Json& value);

[[nodiscard]] Json encode(const metadata::ArtworkInventoryPolicy& policy);
[[nodiscard]] core::Result<metadata::ArtworkInventoryPolicy>
decode_inventory_policy(const Json& value);

[[nodiscard]] Json encode(const metadata::LocalArtworkInventory& inventory);
[[nodiscard]] core::Result<metadata::LocalArtworkInventory> decode_inventory(const Json& value);

[[nodiscard]] Json encode(const metadata::ArtworkWritePlanSource& source);
[[nodiscard]] core::Result<metadata::ArtworkWritePlanSource>
decode_artwork_source(const Json& value);

[[nodiscard]] Json encode(const metadata::ArtworkWritePlan& plan);
[[nodiscard]] core::Result<metadata::ArtworkWritePlan> decode_artwork_plan(const Json& value);

[[nodiscard]] Json encode(const operations::ArtworkApplyResult& result);
[[nodiscard]] core::Result<operations::ArtworkApplyResult>
decode_artwork_apply_result(const Json& value);

[[nodiscard]] Json encode(const operations::ArtworkApplyProgress& progress);
[[nodiscard]] core::Result<operations::ArtworkApplyProgress>
decode_artwork_apply_progress(const Json& value);

// ADR-0237 stage 5: moves and renames. The path plan is built by the client
// from the saved layout and destination; the engine observes its filesystem
// for it (the preflight) and publishes a reviewed preparation -- tags, paths,
// or both at once.
[[nodiscard]] Json encode(const operations::OutputPathPlan& plan);
[[nodiscard]] core::Result<operations::OutputPathPlan> decode_path_plan(const Json& value);

[[nodiscard]] Json encode(const operations::OutputPathPreflight& preflight);
[[nodiscard]] core::Result<operations::OutputPathPreflight>
decode_path_preflight(const Json& value);

[[nodiscard]] Json encode(const operations::PreparationPlan& plan);
[[nodiscard]] core::Result<operations::PreparationPlan> decode_preparation_plan(const Json& value);

[[nodiscard]] Json encode(const operations::FilePublicationCommitResult& commit);
[[nodiscard]] core::Result<operations::FilePublicationCommitResult>
decode_publication_commit(const Json& value);

[[nodiscard]] Json encode(const operations::FilePublicationApplyResult& result);
[[nodiscard]] core::Result<operations::FilePublicationApplyResult>
decode_publication_apply_result(const Json& value);

[[nodiscard]] Json encode(const operations::FilePublicationApplyProgress& progress);
[[nodiscard]] core::Result<operations::FilePublicationApplyProgress>
decode_publication_apply_progress(const Json& value);

// Naming layouts and move destinations as saved: {id, profile}.
[[nodiscard]] Json encode(const persistence::SavedOutputLayoutProfile& saved);
[[nodiscard]] core::Result<persistence::SavedOutputLayoutProfile>
decode_saved_layout(const Json& value);
[[nodiscard]] Json encode(const persistence::SavedDestinationProfile& saved);
[[nodiscard]] core::Result<persistence::SavedDestinationProfile>
decode_saved_destination(const Json& value);

} // namespace trackknife::engine::wire
