// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/workspace.hpp"

namespace trackknife::engine {

core::Result<Workspace> Workspace::open(const std::filesystem::path& database) {
    auto repository = persistence::ListRepository::open(database);
    if (!repository) {
        return std::unexpected(std::move(repository.error()));
    }
    return Workspace{std::move(*repository)};
}

unsigned Workspace::schema_version() const {
    const std::lock_guard guard{*mutex_};
    const auto version = repository_.schema_version();
    return version ? *version : 0U;
}

core::Result<std::vector<persistence::ListDocument>> Workspace::load_all() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_all();
}

core::Result<void> Workspace::replace_all(std::span<const persistence::ListDocument> documents) {
    const std::lock_guard guard{*mutex_};
    return repository_.replace_all(documents);
}

core::Result<persistence::LocalMetadataRefreshResult>
Workspace::refresh_local_metadata(const persistence::LocalMetadataRefresh& refresh) {
    const std::lock_guard guard{*mutex_};
    return repository_.refresh_local_metadata(refresh);
}

core::Result<persistence::LocalSourceRelocationResult>
Workspace::relocate_local_source(const persistence::LocalSourceRelocation& relocation) {
    const std::lock_guard guard{*mutex_};
    return repository_.relocate_local_source(relocation);
}

core::Result<std::vector<persistence::ConnectionProfile>> Workspace::load_profiles() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_profiles();
}

core::Result<void>
Workspace::replace_profiles(std::span<const persistence::ConnectionProfile> profiles) {
    const std::lock_guard guard{*mutex_};
    return repository_.replace_profiles(profiles);
}

core::Result<std::vector<persistence::TrackViewPreset>> Workspace::load_view_presets() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_view_presets();
}

core::Result<void>
Workspace::replace_view_presets(std::span<const persistence::TrackViewPreset> presets) {
    const std::lock_guard guard{*mutex_};
    return repository_.replace_view_presets(presets);
}

core::Result<std::vector<persistence::SavedMetadataTransformationChain>>
Workspace::load_metadata_transformation_chains() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_metadata_transformation_chains();
}

core::Result<void> Workspace::upsert_metadata_transformation_chain(
    const persistence::SavedMetadataTransformationChain& saved_chain) {
    const std::lock_guard guard{*mutex_};
    return repository_.upsert_metadata_transformation_chain(saved_chain);
}

core::Result<void> Workspace::remove_metadata_transformation_chain(const core::StableId& id) {
    const std::lock_guard guard{*mutex_};
    return repository_.remove_metadata_transformation_chain(id);
}

core::Result<std::vector<persistence::SavedOutputLayoutProfile>>
Workspace::load_output_layout_profiles() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_output_layout_profiles();
}

core::Result<void> Workspace::upsert_output_layout_profile(
    const persistence::SavedOutputLayoutProfile& saved_profile) {
    const std::lock_guard guard{*mutex_};
    return repository_.upsert_output_layout_profile(saved_profile);
}

core::Result<void> Workspace::remove_output_layout_profile(const core::StableId& id) {
    const std::lock_guard guard{*mutex_};
    return repository_.remove_output_layout_profile(id);
}

core::Result<std::vector<persistence::SavedDestinationProfile>>
Workspace::load_destination_profiles() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_destination_profiles();
}

core::Result<void>
Workspace::upsert_destination_profile(const persistence::SavedDestinationProfile& saved_profile) {
    const std::lock_guard guard{*mutex_};
    return repository_.upsert_destination_profile(saved_profile);
}

core::Result<void> Workspace::remove_destination_profile(const core::StableId& id) {
    const std::lock_guard guard{*mutex_};
    return repository_.remove_destination_profile(id);
}

core::Result<std::vector<persistence::SavedEncoderPreset>> Workspace::load_encoder_presets() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_encoder_presets();
}

core::Result<void>
Workspace::upsert_encoder_preset(const persistence::SavedEncoderPreset& saved_preset) {
    const std::lock_guard guard{*mutex_};
    return repository_.upsert_encoder_preset(saved_preset);
}

core::Result<void> Workspace::remove_encoder_preset(const core::StableId& id) {
    const std::lock_guard guard{*mutex_};
    return repository_.remove_encoder_preset(id);
}

core::Result<std::optional<persistence::LocalListeningHistory>>
Workspace::lookup_local_listening_history(const persistence::ListItem& source) const {
    const std::lock_guard guard{*mutex_};
    return repository_.lookup_local_listening_history(source);
}

core::Result<std::string> Workspace::local_listening_key(const persistence::ListItem& source) {
    const std::lock_guard guard{*mutex_};
    return repository_.local_listening_key(source);
}

core::Result<void> Workspace::record_local_listen(const persistence::ListItem& source,
                                                  core::StableId occurrence_id,
                                                  std::int64_t played_at_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.record_local_listen(source, std::move(occurrence_id), played_at_ms);
}

core::Result<void> Workspace::save_local_resume(const std::string_view track_hash,
                                                const std::int64_t position_ms,
                                                const std::int64_t updated_at_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.save_local_resume(track_hash, position_ms, updated_at_ms);
}

core::Result<void> Workspace::save_engine_state(const std::string_view key,
                                                const std::string_view value,
                                                const std::int64_t updated_at_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.save_engine_state(key, value, updated_at_ms);
}

core::Result<std::optional<std::string>>
Workspace::load_engine_state(const std::string_view key) const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_engine_state(key);
}

core::Result<std::vector<persistence::EngineListSummary>> Workspace::load_engine_lists() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_engine_lists();
}

core::Result<std::optional<persistence::EngineList>>
Workspace::load_engine_list(const core::StableId& id) const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_engine_list(id);
}

core::Result<persistence::EngineListSummary>
Workspace::save_engine_list(const core::StableId& id, const std::string_view name,
                         const persistence::EngineListKind kind,
                         const std::vector<persistence::EngineListItem>& items,
                         const std::optional<std::uint64_t> expected_revision,
                         const std::int64_t now_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.save_engine_list(id, name, kind, items, expected_revision, now_ms);
}

core::Result<persistence::EngineListSummary>
Workspace::rename_engine_list(const core::StableId& id, const std::string_view name,
                           const std::optional<std::uint64_t> expected_revision,
                           const std::int64_t now_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.rename_engine_list(id, name, expected_revision, now_ms);
}

core::Result<bool> Workspace::delete_engine_list(const core::StableId& id,
                                              const std::optional<std::uint64_t> expected_revision) {
    const std::lock_guard guard{*mutex_};
    return repository_.delete_engine_list(id, expected_revision);
}

core::Result<std::vector<persistence::EngineListSummary>>
Workspace::relocate_engine_list_paths(const std::vector<std::pair<std::string, std::string>>& moves,
                                      const std::int64_t now_ms) {
    const std::lock_guard guard{*mutex_};
    return repository_.relocate_engine_list_paths(moves, now_ms);
}

core::Result<std::vector<persistence::SavedSearch>> Workspace::load_saved_searches() const {
    const std::lock_guard guard{*mutex_};
    return repository_.load_saved_searches();
}

core::Result<void> Workspace::save_search(const persistence::SavedSearch& search) {
    const std::lock_guard guard{*mutex_};
    return repository_.save_search(search);
}

core::Result<void> Workspace::remove_search(const persistence::SavedSearch& expected) {
    const std::lock_guard guard{*mutex_};
    return repository_.remove_search(expected);
}

} // namespace trackknife::engine
