// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/preset.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/transformation.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace trackknife::persistence {

// mpd documents are client-owned working lists of server tracks
// (ADR-0181): every item is ListSource::mpd, rendered from its snapshot.
enum class ListKind : std::uint8_t { scratch, saved, mpd };
enum class ListSource : std::uint8_t { mpd, local };

struct SnapshotField {
    std::string name;
    std::string value;
    std::string native_name{};
    metadata::FieldProvenance provenance{metadata::FieldProvenance::cached_snapshot};
    std::optional<std::string> language{};
    std::optional<std::string> description{};

    friend bool operator==(const SnapshotField&, const SnapshotField&) = default;
};

struct ListItemSegment {
    std::int64_t start_sample{0};
    std::optional<std::int64_t> end_sample;

    friend bool operator==(const ListItemSegment&, const ListItemSegment&) = default;
};

struct ListItemSourceSelection {
    std::optional<int> audio_stream_index;
    std::optional<int> subsong_index;

    friend bool operator==(const ListItemSourceSelection&,
                           const ListItemSourceSelection&) = default;
};

struct ListItem {
    // ADR-0221: identity of this entry within its list, distinct from the
    // track it points at. Assigned on construction so every in-memory entry
    // is addressable immediately, replaced by the persisted value on load,
    // and preserved across reordering because ordering lives in the separate
    // position column.
    //
    // Deliberately excluded from equality below: two entries for the same
    // track are equal in value and distinct in identity. Comparisons in this
    // codebase ask whether two rows describe the same track, and that meaning
    // must not change.
    core::StableId entry_id{core::StableId::random()};
    ListSource source{ListSource::mpd};
    std::optional<core::StableId> profile_id;
    // MPD URIs and local raw OS paths are stored as SQLite BLOBs. No UTF-8 or
    // URL interpretation occurs at this boundary.
    std::string source_reference;
    // Opaque logical identity (for example, cue sheet + file/track indexes)
    // remains distinct from the physical source and survives duplicate paths.
    std::optional<std::string> logical_reference;
    std::optional<ListItemSegment> segment;
    // Explicit decoder selection for independently playable content inside a
    // physical source. Ordinary rows leave this absent.
    std::optional<ListItemSourceSelection> source_selection;
    std::optional<std::int64_t> duration_ms;
    std::optional<core::LocalSourceRevision> source_revision{};
    std::vector<SnapshotField> fields;

    // Value equality over everything but entry_id; see the note above.
    friend bool operator==(const ListItem& left, const ListItem& right) {
        return std::tie(left.source, left.profile_id, left.source_reference, left.logical_reference,
                        left.segment, left.source_selection, left.duration_ms, left.source_revision,
                        left.fields) ==
               std::tie(right.source, right.profile_id, right.source_reference,
                        right.logical_reference, right.segment, right.source_selection,
                        right.duration_ms, right.source_revision, right.fields);
    }
};

struct ListDocument {
    core::StableId id;
    ListKind kind{ListKind::scratch};
    std::string name;
    bool pinned{false};
    bool dirty{false};
    std::vector<ListItem> items;

    friend bool operator==(const ListDocument&, const ListDocument&) = default;
};

struct ConnectionProfile {
    core::StableId id;
    std::string name;
    std::string host;
    unsigned port{6600U};
    std::optional<std::string> local_music_root;
    bool auto_connect{false};

    friend bool operator==(const ConnectionProfile&, const ConnectionProfile&) = default;
};

struct TrackViewPreset {
    std::string binding;
    std::string header_state;

    friend bool operator==(const TrackViewPreset&, const TrackViewPreset&) = default;
};

struct SavedMetadataTransformationChain {
    core::StableId id;
    metadata::MetadataTransformationChain chain;
    bool automatic{false};

    friend bool operator==(const SavedMetadataTransformationChain&,
                           const SavedMetadataTransformationChain&) = default;
};

struct SavedOutputLayoutProfile {
    core::StableId id;
    operations::OutputLayoutProfile profile;

    friend bool operator==(const SavedOutputLayoutProfile&,
                           const SavedOutputLayoutProfile&) = default;
};

struct SavedDestinationProfile {
    core::StableId id;
    operations::DestinationProfile profile;

    friend bool operator==(const SavedDestinationProfile&,
                           const SavedDestinationProfile&) = default;
};

// A user-defined encode target beside the built-in presets. Editing any
// preset saves a new profile; built-ins are immutable.
struct SavedEncoderPreset {
    core::StableId id;
    convert::EncoderPreset preset;

    friend bool operator==(const SavedEncoderPreset&, const SavedEncoderPreset&) = default;
};

// Versioned query definitions; result lists remain independent snapshots.
enum class SavedSearchScope : std::uint8_t { library, current_tab, server };
struct SavedSearch {
    core::StableId id;
    std::string name;
    std::string expression;
    std::string dialect{"tkq-1"};
    SavedSearchScope scope{SavedSearchScope::library};
    std::uint64_t revision{0U}; // zero creates; loaded revisions gate updates/deletion
    friend bool operator==(const SavedSearch&, const SavedSearch&) = default;
};

// Local listening state uses a repository-owned source identity plus decoder
// selection/range. Verified publications preserve identity (ADR-0207).
struct LocalListeningHistory {
    std::string track_hash;
    std::uint64_t play_count{0U};
    std::int64_t last_played_ms{0};
    std::int64_t resume_position_ms{0};
    std::int64_t updated_at_ms{0};

    friend bool operator==(const LocalListeningHistory&, const LocalListeningHistory&) = default;
};

// Pure identity projection shared by repository and consistent query snapshots.
[[nodiscard]] core::Result<std::string> local_listening_observation(const ListItem& source);
[[nodiscard]] std::string local_listening_track_hash(const std::string& source_id,
                                                     const ListItem& source);

struct LocalMetadataRefresh {
    core::StableId operation_id;
    std::string source_reference;
    core::LocalSourceRevision previous_revision;
    core::LocalSourceRevision published_revision;
    metadata::MetadataDocument document;
};

struct LocalMetadataRefreshResult {
    std::size_t affected_occurrences{0U};
    bool already_applied{false};

    friend bool operator==(const LocalMetadataRefreshResult&,
                           const LocalMetadataRefreshResult&) = default;
};

struct LocalSourceRelocation {
    core::StableId operation_id;
    std::string source_reference;
    std::string target_reference;
    core::LocalSourceRevision previous_revision;
    core::LocalSourceRevision published_revision;
    // Present only when publication changed embedded content. The same
    // transaction then replaces embedded/stream occurrence fields and the
    // destination metadata cache instead of copying stale source metadata.
    std::optional<metadata::MetadataDocument> published_document;
};

struct LocalSourceRelocationResult {
    std::size_t affected_occurrences{0U};
    bool cache_rekeyed{false};
    bool metadata_refreshed{false};
    bool already_applied{false};

    friend bool operator==(const LocalSourceRelocationResult&,
                           const LocalSourceRelocationResult&) = default;
};

class ListRepository final {
  public:
    ListRepository(ListRepository&&) noexcept;
    ListRepository& operator=(ListRepository&&) noexcept;
    ListRepository(const ListRepository&) = delete;
    ListRepository& operator=(const ListRepository&) = delete;
    ~ListRepository();

    [[nodiscard]] static core::Result<ListRepository> open(const std::filesystem::path& path);

    [[nodiscard]] core::Result<unsigned> schema_version() const;
    [[nodiscard]] core::Result<std::vector<ListDocument>> load_all() const;
    [[nodiscard]] core::Result<void> replace_all(std::span<const ListDocument> documents);
    // Atomically refreshes every local occurrence of one physical source and
    // its source cache. The operation identity makes recovery replay a no-op.
    [[nodiscard]] core::Result<LocalMetadataRefreshResult>
    refresh_local_metadata(const LocalMetadataRefresh& refresh);
    // Atomically re-keys every revision-matching local occurrence and its
    // source cache. When published_document is present, the same transaction
    // also replaces embedded/stream occurrence fields and the destination
    // cache with the published document. Durable relocation history prevents
    // a delayed workspace snapshot from resurrecting earlier path or metadata
    // state and makes recovery replay a no-op.
    [[nodiscard]] core::Result<LocalSourceRelocationResult>
    relocate_local_source(const LocalSourceRelocation& relocation);
    [[nodiscard]] core::Result<std::vector<ConnectionProfile>> load_profiles() const;
    [[nodiscard]] core::Result<void> replace_profiles(std::span<const ConnectionProfile> profiles);
    [[nodiscard]] core::Result<std::vector<TrackViewPreset>> load_view_presets() const;
    [[nodiscard]] core::Result<void> replace_view_presets(std::span<const TrackViewPreset> presets);
    [[nodiscard]] core::Result<std::vector<SavedMetadataTransformationChain>>
    load_metadata_transformation_chains() const;
    [[nodiscard]] core::Result<void>
    upsert_metadata_transformation_chain(const SavedMetadataTransformationChain& saved_chain);
    [[nodiscard]] core::Result<void> remove_metadata_transformation_chain(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<SavedOutputLayoutProfile>>
    load_output_layout_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_output_layout_profile(const SavedOutputLayoutProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_output_layout_profile(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<SavedDestinationProfile>>
    load_destination_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_destination_profile(const SavedDestinationProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_destination_profile(const core::StableId& id);

    [[nodiscard]] core::Result<std::vector<SavedEncoderPreset>> load_encoder_presets() const;
    [[nodiscard]] core::Result<void> upsert_encoder_preset(const SavedEncoderPreset& saved_preset);
    [[nodiscard]] core::Result<void> remove_encoder_preset(const core::StableId& id);

    [[nodiscard]] core::Result<std::vector<SavedSearch>> load_saved_searches() const;
    [[nodiscard]] core::Result<void> save_search(const SavedSearch& search);
    [[nodiscard]] core::Result<void> remove_search(const SavedSearch& expected);

    [[nodiscard]] core::Result<std::optional<LocalListeningHistory>>
    load_local_listening_history(std::string_view track_hash) const;
    // Read-only lookup: an unseen qualified source has no history; never creates identity rows.
    [[nodiscard]] core::Result<std::optional<LocalListeningHistory>>
    lookup_local_listening_history(const ListItem& source) const;
    // Resolves a revision-qualified local source; may create its durable identity.
    [[nodiscard]] core::Result<std::string> local_listening_key(const ListItem& source);
    // Atomically deduplicates a qualified playback occurrence and increments history.
    [[nodiscard]] core::Result<void> record_local_listen(const ListItem& source,
                                                         core::StableId occurrence_id,
                                                         std::int64_t played_at_ms);
    // Each call records one distinct qualified listen. Callers must serialize
    // delivery and must not retry a write with an ambiguous outcome.
    [[nodiscard]] core::Result<void> record_local_play(std::string_view track_hash,
                                                       std::int64_t played_at_ms);
    [[nodiscard]] core::Result<void> save_local_resume(std::string_view track_hash,
                                                       std::int64_t position_ms,
                                                       std::int64_t updated_at_ms);

  private:
    struct Impl;
    explicit ListRepository(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::persistence
