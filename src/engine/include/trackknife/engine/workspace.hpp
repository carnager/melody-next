// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::engine {

// The core's front door for the workspace: lists, connection profiles, view
// presets, saved transformation/output/encoder settings, and local listening
// history.
//
// ADR-0220: the UI asks the core to do something; the core owns the data.
// Unlike engine::Catalogue, which opens a connection per call, the workspace
// database is held open -- it is written on nearly every user action and
// reopening per save would be wasteful. That difference is an implementation
// choice behind the door, not something a caller sees.
//
// Every call is synchronous and mentions no Qt. Callers that need asynchrony
// supply it: in-process ui::ListPersistenceService serialises these on its own
// thread, and in Phase 2 the same operations become protocol requests.
class Workspace final {
  public:
    // Opens the workspace database, applying migrations.
    [[nodiscard]] static core::Result<Workspace> open(const std::filesystem::path& database);

    [[nodiscard]] unsigned schema_version() const;

    [[nodiscard]] core::Result<std::vector<persistence::ListDocument>> load_all() const;
    [[nodiscard]] core::Result<void>
    replace_all(std::span<const persistence::ListDocument> documents);
    [[nodiscard]] core::Result<persistence::LocalMetadataRefreshResult>
    refresh_local_metadata(const persistence::LocalMetadataRefresh& refresh);
    [[nodiscard]] core::Result<persistence::LocalSourceRelocationResult>
    relocate_local_source(const persistence::LocalSourceRelocation& relocation);
    [[nodiscard]] core::Result<std::vector<persistence::ConnectionProfile>> load_profiles() const;
    [[nodiscard]] core::Result<void>
    replace_profiles(std::span<const persistence::ConnectionProfile> profiles);
    [[nodiscard]] core::Result<std::vector<persistence::TrackViewPreset>> load_view_presets() const;
    [[nodiscard]] core::Result<void>
    replace_view_presets(std::span<const persistence::TrackViewPreset> presets);
    [[nodiscard]] core::Result<std::vector<persistence::SavedMetadataTransformationChain>>
    load_metadata_transformation_chains() const;
    [[nodiscard]] core::Result<void> upsert_metadata_transformation_chain(
        const persistence::SavedMetadataTransformationChain& saved_chain);
    [[nodiscard]] core::Result<void> remove_metadata_transformation_chain(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<persistence::SavedOutputLayoutProfile>>
    load_output_layout_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_output_layout_profile(const persistence::SavedOutputLayoutProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_output_layout_profile(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<persistence::SavedDestinationProfile>>
    load_destination_profiles() const;
    [[nodiscard]] core::Result<void>
    upsert_destination_profile(const persistence::SavedDestinationProfile& saved_profile);
    [[nodiscard]] core::Result<void> remove_destination_profile(const core::StableId& id);
    [[nodiscard]] core::Result<std::vector<persistence::SavedEncoderPreset>>
    load_encoder_presets() const;
    [[nodiscard]] core::Result<void>
    upsert_encoder_preset(const persistence::SavedEncoderPreset& saved_preset);
    [[nodiscard]] core::Result<void> remove_encoder_preset(const core::StableId& id);
    [[nodiscard]] core::Result<std::optional<persistence::LocalListeningHistory>>
    lookup_local_listening_history(const persistence::ListItem& source) const;
    [[nodiscard]] core::Result<std::string>
    local_listening_key(const persistence::ListItem& source);
    [[nodiscard]] core::Result<void> record_local_listen(const persistence::ListItem& source,
                                                         core::StableId occurrence_id,
                                                         std::int64_t played_at_ms);

    // Where a track was left, keyed by content identity so it survives the
    // file being re-encoded.
    [[nodiscard]] core::Result<void> save_local_resume(std::string_view track_hash,
                                                       std::int64_t position_ms,
                                                       std::int64_t updated_at_ms);

    // Saved searches. A saved search is user-authored, so removal is checked
    // against the expected definition rather than a bare name.
    // ADR-0220: what the engine remembers between runs -- its queue, what was
    // playing, where it had got to. Opaque to the workspace: the queue is the
    // player's business and this is only where it is kept.
    [[nodiscard]] core::Result<void> save_engine_state(std::string_view key, std::string_view value,
                                                       std::int64_t updated_at_ms);
    [[nodiscard]] core::Result<std::optional<std::string>>
    load_engine_state(std::string_view key) const;

    // ADR-0233: lists, the engine's own -- working and saved.
    [[nodiscard]] core::Result<std::vector<persistence::EngineListSummary>> load_engine_lists() const;
    [[nodiscard]] core::Result<std::optional<persistence::EngineList>>
    load_engine_list(const core::StableId& id) const;
    [[nodiscard]] core::Result<persistence::EngineListSummary>
    save_engine_list(const core::StableId& id, std::string_view name,
                  persistence::EngineListKind kind,
                  const std::vector<persistence::EngineListItem>& items,
                  std::optional<std::uint64_t> expected_revision, std::int64_t now_ms);
    [[nodiscard]] core::Result<persistence::EngineListSummary>
    rename_engine_list(const core::StableId& id, std::string_view name,
                    std::optional<std::uint64_t> expected_revision, std::int64_t now_ms);
    [[nodiscard]] core::Result<bool> delete_engine_list(const core::StableId& id,
                                                     std::optional<std::uint64_t> expected_revision);

    [[nodiscard]] core::Result<std::vector<persistence::SavedSearch>> load_saved_searches() const;
    [[nodiscard]] core::Result<void> save_search(const persistence::SavedSearch& search);
    [[nodiscard]] core::Result<void> remove_search(const persistence::SavedSearch& expected);

  private:
    explicit Workspace(persistence::ListRepository repository)
        : repository_(std::move(repository)) {}

    persistence::ListRepository repository_;
    // One call at a time. The engine calls in from its recorder, its
    // playback store and every client's connection (ADR-0233), and the
    // repository's writes are transactions on one SQLite connection: two
    // threads' BEGINs on it would nest, and one would fail or take the
    // other's statements in. Behind a pointer so the workspace still moves.
    std::unique_ptr<std::mutex> mutex_{std::make_unique<std::mutex>()};
};

} // namespace trackknife::engine
