// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void list_documents_round_trip_transactionally() {
    namespace persistence = trackknife::persistence;
    const auto database_path = std::filesystem::temp_directory_path() /
                               ("trackknife-list-repository-" +
                                trackknife::core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto profile_id = trackknife::core::StableId::random();
    const auto scratch_id = trackknife::core::StableId::random();
    const auto saved_id = trackknife::core::StableId::random();
    const std::string raw_local_path{"music/invalid-\xff.flac", 20U};
    const std::vector<persistence::ListDocument> expected{
        persistence::ListDocument{
            .id = scratch_id,
            .kind = persistence::ListKind::scratch,
            .name = "Scratch 1",
            .pinned = false,
            .dirty = true,
            .items =
                {
                    persistence::ListItem{
                        .source = persistence::ListSource::mpd,
                        .profile_id = profile_id,
                        .source_reference = "Artist/Album/01.flac",
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = 180'125,
                        .fields = {{"Artist", "First"}, {"Artist", "Second"}, {"Title", "Song"}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::mpd,
                        .profile_id = profile_id,
                        .source_reference = "Artist/Album/01.flac",
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = 180'125,
                        .fields = {{"Artist", "First"}, {"Title", "Song"}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = raw_local_path,
                        .logical_reference = std::string{"sheet.cue\0file:0\0track:0", 24U},
                        .segment =
                            persistence::ListItemSegment{
                                .start_sample = 44'100,
                                .end_sample = 88'200,
                            },
                        .source_selection =
                            persistence::ListItemSourceSelection{
                                .audio_stream_index = 0,
                                .subsong_index = 3,
                            },
                        .duration_ms = std::nullopt,
                        .fields = {{"Title", "Raw path"}},
                    },
                },
        },
        persistence::ListDocument{
            .id = saved_id,
            .kind = persistence::ListKind::saved,
            .name = "Tag work",
            .pinned = true,
            .dirty = false,
            .items = {},
        },
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        if (!opened) {
            std::cerr << opened.error().message << '\n';
        }
        require(opened.has_value(), "list repository must create and migrate a new database");
        auto repository = std::move(*opened);
        require(repository.schema_version() == 31U, "state repository schema must be explicit");
        require(repository.replace_all(expected).has_value(),
                "valid list documents must commit in one transaction");
        require(repository.load_all() == expected,
                "list order, duplicates, raw paths, and repeated snapshot fields must round trip");

        auto invalid = expected;
        invalid.back().id = scratch_id;
        const auto rejected = repository.replace_all(invalid);
        require(!rejected && rejected.error().code == trackknife::core::ErrorCode::invalid_argument,
                "duplicate document IDs must be rejected before starting a write");
        require(repository.load_all() == expected,
                "a rejected replacement must leave the previous transaction intact");

        const std::vector profiles{
            persistence::ConnectionProfile{
                .id = profile_id,
                .name = "Melody",
                .host = "caprica",
                .port = 6600U,
                .local_music_root = std::string{"/srv/music"},
                .auto_connect = true,
            },
            persistence::ConnectionProfile{
                .id = trackknife::core::StableId::random(),
                .name = "Stock MPD",
                .host = "127.0.0.1",
                .port = 6601U,
                .local_music_root = std::nullopt,
                .auto_connect = false,
            },
        };
        require(repository.replace_profiles(profiles).has_value() &&
                    repository.load_profiles() == profiles,
                "connection profiles must round trip in stable display order");
        auto conflicting_profiles = profiles;
        conflicting_profiles.back().auto_connect = true;
        require(!repository.replace_profiles(conflicting_profiles) &&
                    repository.load_profiles() == profiles,
                "profile validation must preserve the preceding transaction");
        const std::vector presets{
            persistence::TrackViewPreset{.binding = "live-queue", .header_state = "qt-state"},
            persistence::TrackViewPreset{.binding = "local:" + scratch_id.to_string(),
                                         .header_state = std::string{"\0\xff", 2U}},
        };
        require(repository.replace_view_presets(presets).has_value() &&
                    repository.load_view_presets() == presets,
                "binary Qt track-view state must round trip transactionally");
        auto invalid_presets = presets;
        invalid_presets.back().binding = presets.front().binding;
        require(!repository.replace_view_presets(invalid_presets) &&
                    repository.load_view_presets() == presets,
                "invalid view presets must preserve the preceding transaction");
    }

    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value() && reopened->load_all() == expected,
                "persisted list documents must survive closing and reopening SQLite");
    }
    cleanup();
}

template <typename Action>
const Action& require_action(const trackknife::metadata::MetadataTransformationChain& chain,
                             const std::size_t index, const std::string_view message) {
    require(index < chain.actions.size(), message);
    const auto* action = std::get_if<Action>(&chain.actions[index]);
    require(action != nullptr, message);
    return *action;
}

void metadata_transformation_chains_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-transformation-chains-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto chain_id = core::StableId::random();
    const persistence::SavedMetadataTransformationChain expected{
        .id = chain_id,
        .chain =
            metadata::MetadataTransformationChain{
                .schema_version = 1U,
                .name = "Exact cleanup",
                .actions =
                    {
                        metadata::MetadataSetValuesAction{.target_field = "Artist",
                                                          .values = {"one", "", "one"}},
                        metadata::MetadataAddValuesAction{.target_field = "Artist",
                                                          .values = {"tail", ""}},
                        metadata::MetadataRemoveFieldAction{
                            .target_field = "Comment:",
                            .match_mode = metadata::MetadataFieldMatchMode::exact_native},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Title",
                            .transform = metadata::MetadataValueTransformKind::trim_ascii},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Album",
                            .transform = metadata::MetadataValueTransformKind::lowercase},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Genre",
                            .transform = metadata::MetadataValueTransformKind::uppercase},
                        metadata::MetadataTransformValuesAction{
                            .target_field = "Subtitle",
                            .transform = metadata::MetadataValueTransformKind::capitalize_first},
                        metadata::MetadataCopyFieldAction{.target_field = "Credits",
                                                          .source_field = "Artist"},
                        metadata::MetadataSplitValuesAction{.target_field = "Tags",
                                                            .separator = "::"},
                        metadata::MetadataJoinValuesAction{.target_field = "Tags", .separator = ""},
                        metadata::MetadataFormatValueAction{
                            .target_field = "Sort title", .dialect = {}, .source = "%artist%"},
                        metadata::MetadataRemoveMatchingValuesAction{.target_field = "Artist",
                                                                     .match = "Unknown"},
                        metadata::MetadataReplaceMatchingValuesAction{
                            .target_field = "Genre",
                            .match = "Rock",
                            .replacement_values = {"Alternative", "Indie"}},
                        metadata::MetadataNumberSelectedItemsAction{
                            .target_field = "Track Number", .start = 7U, .padding = 2U},
                        metadata::MetadataKeepFirstCharactersAction{.target_field = "Date",
                                                                    .character_count = 4U},
                        metadata::MetadataRemoveFieldIfAction{
                            .target_field = "Disc Number",
                            .dialect = {},
                            .condition = "$not(%totaldiscs%)",
                            .match_mode = metadata::MetadataFieldMatchMode::exact_native,
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::filename,
                            .source = {},
                            .pattern = "%tracknumber%. %title%",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::full_path,
                            .source = {},
                            .pattern = "%directory%/%title%.flac",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::formatted,
                            .source = "%artist% — %title%",
                            .pattern = "%displayartist% — %displaytitle%",
                        },
                        metadata::MetadataCaptureValuesAction{
                            .dialect = {},
                            .source_kind = metadata::MetadataCaptureSourceKind::field,
                            .source = "Comment",
                            .pattern = "%note%",
                        },
                        metadata::MetadataNumberGroupedItemsAction{
                            .target_field = "Track Number",
                            .dialect = {},
                            .group_expression = "%album%|%discnumber%",
                            .start = 1U,
                            .padding = 2U,
                        },
                    },
            },
        .automatic = true,
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "transformation repository must open");
        auto repository = std::move(*opened);
        require(repository.upsert_metadata_transformation_chain(expected).has_value(),
                "a validated transformation chain must persist atomically");
        const auto loaded = repository.load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U && loaded->front().id == chain_id &&
                    loaded->front().chain.schema_version == 1U &&
                    loaded->front().chain.name == expected.chain.name &&
                    loaded->front().automatic &&
                    loaded->front().chain.actions.size() == expected.chain.actions.size(),
                "saved transformation identity, schema, automatic policy, name, and action count "
                "must round trip");
        const auto& chain = loaded->front().chain;
        require(
            require_action<metadata::MetadataSetValuesAction>(chain, 0U, "set action") ==
                    require_action<metadata::MetadataSetValuesAction>(expected.chain, 0U,
                                                                      "expected set action") &&
                require_action<metadata::MetadataAddValuesAction>(chain, 1U, "add action") ==
                    require_action<metadata::MetadataAddValuesAction>(expected.chain, 1U,
                                                                      "expected add action") &&
                require_action<metadata::MetadataRemoveFieldAction>(chain, 2U, "remove action") ==
                    require_action<metadata::MetadataRemoveFieldAction>(expected.chain, 2U,
                                                                        "expected remove action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 3U, "trim action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 3U, "expected trim action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 4U,
                                                                        "lower action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 4U, "expected lower action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 5U,
                                                                        "upper action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 5U, "expected upper action") &&
                require_action<metadata::MetadataTransformValuesAction>(chain, 6U,
                                                                        "capitalize action") ==
                    require_action<metadata::MetadataTransformValuesAction>(
                        expected.chain, 6U, "expected capitalize action") &&
                require_action<metadata::MetadataCopyFieldAction>(chain, 7U, "copy action") ==
                    require_action<metadata::MetadataCopyFieldAction>(expected.chain, 7U,
                                                                      "expected copy action") &&
                require_action<metadata::MetadataSplitValuesAction>(chain, 8U, "split action") ==
                    require_action<metadata::MetadataSplitValuesAction>(expected.chain, 8U,
                                                                        "expected split action") &&
                require_action<metadata::MetadataJoinValuesAction>(chain, 9U, "join action") ==
                    require_action<metadata::MetadataJoinValuesAction>(expected.chain, 9U,
                                                                       "expected join action") &&
                require_action<metadata::MetadataFormatValueAction>(chain, 10U, "format action") ==
                    require_action<metadata::MetadataFormatValueAction>(expected.chain, 10U,
                                                                        "expected format action") &&
                require_action<metadata::MetadataRemoveMatchingValuesAction>(
                    chain, 11U, "remove matching action") ==
                    require_action<metadata::MetadataRemoveMatchingValuesAction>(
                        expected.chain, 11U, "expected remove matching action") &&
                require_action<metadata::MetadataReplaceMatchingValuesAction>(
                    chain, 12U, "replace matching action") ==
                    require_action<metadata::MetadataReplaceMatchingValuesAction>(
                        expected.chain, 12U, "expected replace matching action") &&
                require_action<metadata::MetadataNumberSelectedItemsAction>(chain, 13U,
                                                                            "number action") ==
                    require_action<metadata::MetadataNumberSelectedItemsAction>(
                        expected.chain, 13U, "expected number action") &&
                require_action<metadata::MetadataKeepFirstCharactersAction>(chain, 14U,
                                                                            "keep-first action") ==
                    require_action<metadata::MetadataKeepFirstCharactersAction>(
                        expected.chain, 14U, "expected keep-first action") &&
                require_action<metadata::MetadataRemoveFieldIfAction>(
                    chain, 15U, "conditional-remove action") ==
                    require_action<metadata::MetadataRemoveFieldIfAction>(
                        expected.chain, 15U, "expected conditional-remove action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 16U,
                                                                      "capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 16U, "expected capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 17U,
                                                                      "full-path capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 17U, "expected full-path capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 18U,
                                                                      "formatted capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 18U, "expected formatted capture action") &&
                require_action<metadata::MetadataCaptureValuesAction>(chain, 19U,
                                                                      "field capture action") ==
                    require_action<metadata::MetadataCaptureValuesAction>(
                        expected.chain, 19U, "expected field capture action") &&
                require_action<metadata::MetadataNumberGroupedItemsAction>(
                    chain, 20U, "grouped numbering action") ==
                    require_action<metadata::MetadataNumberGroupedItemsAction>(
                        expected.chain, 20U, "expected grouped numbering action"),
            "explicit action kinds and exact ordered payloads must round trip");

        auto conflicting = expected;
        conflicting.id = core::StableId::random();
        const auto conflict = repository.upsert_metadata_transformation_chain(conflicting);
        require(!conflict && conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_metadata_transformation_chains()->size() == 1U,
                "an exact duplicate saved name must preserve the preceding transaction");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "transformation repository must reopen");
        auto loaded = reopened->load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U &&
                    loaded->front().chain.actions.size() == expected.chain.actions.size() &&
                    require_action<metadata::MetadataKeepFirstCharactersAction>(
                        loaded->front().chain, 14U, "reopened keep-first action") ==
                        require_action<metadata::MetadataKeepFirstCharactersAction>(
                            expected.chain, 14U, "expected reopened keep-first action") &&
                    require_action<metadata::MetadataRemoveFieldIfAction>(
                        loaded->front().chain, 15U, "reopened conditional-remove action") ==
                        require_action<metadata::MetadataRemoveFieldIfAction>(
                            expected.chain, 15U, "expected reopened conditional-remove action") &&
                    require_action<metadata::MetadataCaptureValuesAction>(
                        loaded->front().chain, 19U, "reopened capture action") ==
                        require_action<metadata::MetadataCaptureValuesAction>(
                            expected.chain, 19U, "expected reopened capture action"),
                "typed numeric and conditional transformations must survive restart");
        auto updated = expected;
        updated.chain.name = "Exact cleanup v2";
        updated.chain.actions = {
            metadata::MetadataJoinValuesAction{.target_field = "Artist", .separator = "; "}};
        require(reopened->upsert_metadata_transformation_chain(updated).has_value(),
                "upserting one stable identity must replace its chain atomically");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "updated transformation repository must reopen");
        auto loaded = reopened->load_metadata_transformation_chains();
        require(loaded.has_value() && loaded->size() == 1U &&
                    loaded->front().chain.name == "Exact cleanup v2" && loaded->front().automatic &&
                    require_action<metadata::MetadataJoinValuesAction>(loaded->front().chain, 0U,
                                                                       "reopened join action")
                            .separator == "; ",
                "updated saved transformations must survive restart");
        require(reopened->remove_metadata_transformation_chain(chain_id).has_value() &&
                    reopened->load_metadata_transformation_chains()->empty(),
                "deleting a saved chain must cascade through its ordered payload");
        const auto missing = reopened->remove_metadata_transformation_chain(chain_id);
        require(!missing && missing.error().code == core::ErrorCode::not_found,
                "deleting an absent saved chain must report not-found");
    }
    cleanup();
}

void output_layout_and_destination_profiles_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace operations = trackknife::operations;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-output-profiles-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto layout_id = core::StableId::random();
    const auto destination_id = core::StableId::random();
    const persistence::SavedOutputLayoutProfile expected_layout{
        .id = layout_id,
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = "Album folders",
                .dialect = {},
                .relative_directory_expression = "%album artist%/%album%",
                .basename_expression = "$num(%tracknumber%,2) - %title%",
                .sanitization_policy = {.name = "linux", .version = 1U},
            },
    };
    const std::string raw_destination{"/srv/music/library-\xff", 20U};
    const persistence::SavedDestinationProfile expected_destination{
        .id = destination_id,
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = "Main library",
                .root_raw_path = raw_destination,
                .containment_policy = {.name = "lexical-beneath-root", .version = 1U},
            },
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "output-profile repository must open");
        auto repository = std::move(*opened);
        require(repository.schema_version() == 31U,
                "output profiles must survive the explicit schema-18 migration");
        require(repository.upsert_output_layout_profile(expected_layout).has_value() &&
                    repository.upsert_destination_profile(expected_destination).has_value(),
                "validated output and destination profiles must persist atomically");
        require(repository.load_output_layout_profiles() == std::vector{expected_layout} &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "versioned expressions, policies, and raw destination bytes must round trip");

        auto conflicting_layout = expected_layout;
        conflicting_layout.id = core::StableId::random();
        const auto layout_conflict = repository.upsert_output_layout_profile(conflicting_layout);
        require(!layout_conflict && layout_conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_output_layout_profiles() == std::vector{expected_layout},
                "duplicate output-layout names must preserve the preceding transaction");

        auto conflicting_destination = expected_destination;
        conflicting_destination.id = core::StableId::random();
        const auto destination_conflict =
            repository.upsert_destination_profile(conflicting_destination);
        require(!destination_conflict &&
                    destination_conflict.error().code == core::ErrorCode::conflict &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "duplicate destination names must preserve the preceding transaction");

        auto invalid_destination = expected_destination;
        invalid_destination.profile.root_raw_path = "relative/library";
        const auto rejected = repository.upsert_destination_profile(invalid_destination);
        require(!rejected && rejected.error().code == core::ErrorCode::invalid_argument &&
                    repository.load_destination_profiles() == std::vector{expected_destination},
                "invalid destination roots must be rejected without changing persisted state");

        auto updated_layout = expected_layout;
        updated_layout.profile.name = "Disc folders";
        updated_layout.profile.relative_directory_expression =
            "%album artist%/%album%/Disc %discnumber%";
        updated_layout.profile.basename_expression = "%tracknumber%. %title%";
        auto updated_destination = expected_destination;
        updated_destination.profile.name = "Archive";
        updated_destination.profile.root_raw_path = "/mnt/archive";
        require(repository.upsert_output_layout_profile(updated_layout).has_value() &&
                    repository.upsert_destination_profile(updated_destination).has_value(),
                "upserting stable profile identities must replace their payloads");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "output-profile repository must reopen");
        auto layouts = reopened->load_output_layout_profiles();
        auto destinations = reopened->load_destination_profiles();
        require(layouts.has_value() && layouts->size() == 1U && layouts->front().id == layout_id &&
                    layouts->front().profile.name == "Disc folders" &&
                    layouts->front().profile.relative_directory_expression ==
                        "%album artist%/%album%/Disc %discnumber%" &&
                    destinations.has_value() && destinations->size() == 1U &&
                    destinations->front().id == destination_id &&
                    destinations->front().profile.name == "Archive" &&
                    destinations->front().profile.root_raw_path == "/mnt/archive",
                "updated output and destination profiles must survive restart");
        require(reopened->remove_output_layout_profile(layout_id).has_value() &&
                    reopened->remove_destination_profile(destination_id).has_value() &&
                    reopened->load_output_layout_profiles()->empty() &&
                    reopened->load_destination_profiles()->empty(),
                "profile deletion must remove exactly the selected stable identities");
        const auto missing_layout = reopened->remove_output_layout_profile(layout_id);
        const auto missing_destination = reopened->remove_destination_profile(destination_id);
        require(!missing_layout && missing_layout.error().code == core::ErrorCode::not_found &&
                    !missing_destination &&
                    missing_destination.error().code == core::ErrorCode::not_found,
                "deleting absent output profiles must report not-found");
    }
    cleanup();
}

void encoder_presets_round_trip_transactionally() {
    namespace core = trackknife::core;
    namespace convert = trackknife::convert;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-encoder-presets-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const auto opus_id = core::StableId::random();
    const auto flac_id = core::StableId::random();
    const persistence::SavedEncoderPreset custom_opus{
        .id = opus_id,
        .preset =
            convert::EncoderPreset{
                .id = opus_id.to_string(),
                .version = 1,
                .display_name = "Opus for the car",
                .codec_name = "libopus",
                .container_name = "opus",
                .file_extension = "opus",
                .lossless = false,
                .bit_rate = 96'000,
                .vbr_quality = std::nullopt,
                .sample_format_hint = {},
            },
    };
    const persistence::SavedEncoderPreset custom_flac{
        .id = flac_id,
        .preset =
            convert::EncoderPreset{
                .id = flac_id.to_string(),
                .version = 1,
                .display_name = "Archive FLAC",
                .codec_name = "flac",
                .container_name = "flac",
                .file_extension = "flac",
                .lossless = true,
                .bit_rate = std::nullopt,
                .vbr_quality = std::nullopt,
                .sample_format_hint = "s32",
            },
    };
    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "encoder-preset repository must open");
        auto repository = std::move(*opened);
        require(repository.load_encoder_presets().value_or(
                    std::vector<persistence::SavedEncoderPreset>{{}}) ==
                    std::vector<persistence::SavedEncoderPreset>{},
                "a fresh repository has no encoder presets");
        require(repository.upsert_encoder_preset(custom_opus).has_value(),
                "custom opus preset must save");
        require(repository.upsert_encoder_preset(custom_flac).has_value(),
                "custom flac preset must save");

        auto name_clash = custom_flac;
        name_clash.id = core::StableId::random();
        name_clash.preset.id = name_clash.id.to_string();
        const auto clashed = repository.upsert_encoder_preset(name_clash);
        require(!clashed.has_value() &&
                    clashed.error().code == trackknife::core::ErrorCode::conflict,
                "duplicate encoder preset names must conflict");

        auto both_rates = custom_opus;
        both_rates.preset.vbr_quality = 2;
        require(!repository.upsert_encoder_preset(both_rates).has_value(),
                "bit rate and VBR quality together must be rejected");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "encoder-preset repository must reopen");
        const auto loaded = reopened->load_encoder_presets();
        require(loaded.has_value() && loaded->size() == 2U,
                "both encoder presets survive a restart");
        require(loaded && loaded->front() == custom_flac && loaded->back() == custom_opus,
                "encoder presets reload exactly, ordered by name");
        require(reopened->remove_encoder_preset(opus_id).has_value(),
                "encoder preset removal must succeed");
        const auto missing = reopened->remove_encoder_preset(opus_id);
        require(!missing.has_value() &&
                    missing.error().code == trackknife::core::ErrorCode::not_found,
                "removing a removed encoder preset reports not_found");
        const auto remaining = reopened->load_encoder_presets();
        require(remaining.has_value() && remaining->size() == 1U &&
                    remaining->front() == custom_flac,
                "the remaining encoder preset is intact");
    }
    cleanup();
}

void committed_metadata_refreshes_every_occurrence_idempotently() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-metadata-refresh-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const std::string source{"/music/invalid-\xff.flac", 21U};
    const std::string other{"/music/other.flac"};
    const auto embedded = metadata::FieldProvenance::embedded;
    const auto annotation = metadata::FieldProvenance::annotation;
    const auto sidecar = metadata::FieldProvenance::sidecar;
    const auto segment = metadata::FieldProvenance::segment;
    const core::LocalSourceRevision previous_revision{
        .device = 10U,
        .inode = 20U,
        .size = 30U,
        .modification_time_seconds = -40,
        .modification_time_nanoseconds = 50,
    };
    const std::vector<persistence::ListDocument> stale_documents{
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "First",
            .pinned = false,
            .dirty = false,
            .items =
                {
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = source,
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = 1'000,
                        .source_revision = previous_revision,
                        .fields =
                            {
                                {.name = "title",
                                 .value = "Old embedded",
                                 .native_name = "TITLE",
                                 .provenance = embedded},
                                {.name = "comment",
                                 .value = "Private note",
                                 .native_name = "COMMENT",
                                 .provenance = annotation},
                            },
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = source,
                        .logical_reference = std::string{"cue-v1\0sheet\0track-1", 20U},
                        .segment =
                            persistence::ListItemSegment{.start_sample = 0, .end_sample = 44'100},
                        .source_selection = std::nullopt,
                        .duration_ms = 1'000,
                        .source_revision = previous_revision,
                        .fields =
                            {
                                {.name = "title",
                                 .value = "Old embedded",
                                 .native_name = "TITLE",
                                 .provenance = embedded},
                                {.name = "album",
                                 .value = "Old album",
                                 .native_name = "ALBUM",
                                 .provenance = embedded},
                                {.name = "title",
                                 .value = "CUE title",
                                 .native_name = "TITLE",
                                 .provenance = sidecar},
                                {.name = "tracknumber",
                                 .value = "1",
                                 .native_name = "TRACKNUMBER",
                                 .provenance = segment},
                            },
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = other,
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = std::nullopt,
                        .source_revision = std::nullopt,
                        .fields = {{.name = "title",
                                    .value = "Other",
                                    .native_name = "TITLE",
                                    .provenance = embedded}},
                    },
                },
        },
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = "Duplicate",
            .pinned = true,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = 1'000,
                .source_revision = previous_revision,
                .fields = {{.name = "title",
                            .value = "Old embedded",
                            .native_name = "TITLE",
                            .provenance = embedded}},
            }},
        },
    };
    const core::LocalSourceRevision published_revision{
        .device = 1U,
        .inode = 2U,
        .size = 3U,
        .modification_time_seconds = -4,
        .modification_time_nanoseconds = 5,
    };
    const auto operation_id = core::StableId::random();
    const persistence::LocalMetadataRefresh refresh{
        .operation_id = operation_id,
        .source_reference = source,
        .previous_revision = previous_revision,
        .published_revision = published_revision,
        .document =
            metadata::MetadataDocument{
                .fields =
                    {
                        metadata::MetadataField{
                            .canonical_name = "title",
                            .native_name = "TITLE",
                            .values = {"New embedded", "Alternate title"},
                            .qualifier =
                                metadata::FieldQualifier{.language = "en", .description = "main"},
                            .provenance = embedded,
                        },
                        metadata::MetadataField{
                            .canonical_name = "album",
                            .native_name = "ALBUM",
                            .values = {"New album"},
                            .qualifier = {},
                            .provenance = embedded,
                        },
                    },
                .unsupported_native_objects = {},
            },
    };

    {
        auto opened = persistence::ListRepository::open(database_path);
        require(opened.has_value(), "metadata refresh repository must open");
        auto repository = std::move(*opened);
        require(repository.replace_all(stale_documents).has_value(),
                "provenance-aware list snapshots must persist");
        const auto applied = repository.refresh_local_metadata(refresh);
        require(applied == persistence::LocalMetadataRefreshResult{.affected_occurrences = 3U,
                                                                   .already_applied = false},
                "one transaction must refresh every duplicate and logical occurrence");
        auto loaded = repository.load_all();
        require(loaded.has_value(), "refreshed list state must load");
        const auto& whole_fields = (*loaded)[0].items[0].fields;
        require(whole_fields.size() == 4U && whole_fields[0].value == "New embedded" &&
                    whole_fields[1].value == "Alternate title" &&
                    whole_fields[0].language == std::optional<std::string>{"en"} &&
                    whole_fields[0].description == std::optional<std::string>{"main"} &&
                    whole_fields.back().value == "Private note" &&
                    whole_fields.back().provenance == annotation,
                "source replacement must preserve qualifiers and non-source annotations");
        const auto& cue_fields = (*loaded)[0].items[1].fields;
        require(cue_fields.size() == 5U && cue_fields[0].value == "New embedded" &&
                    cue_fields[2].value == "New album" && cue_fields[3].value == "CUE title" &&
                    cue_fields[3].provenance == sidecar && cue_fields[4].value == "1" &&
                    cue_fields[4].provenance == segment,
                "CUE and segment overlays must survive an embedded source refresh");
        require((*loaded)[0].items[2] == stale_documents[0].items[2],
                "unrelated local sources must remain byte-for-byte unchanged");

        require(repository.replace_all(stale_documents).has_value(),
                "an already queued stale workspace save may still arrive");
        const auto replayed = repository.refresh_local_metadata(refresh);
        require(replayed == persistence::LocalMetadataRefreshResult{.affected_occurrences = 3U,
                                                                    .already_applied = true},
                "recovery replay must be an idempotent no-op");
        loaded = repository.load_all();
        require(loaded.has_value() && (*loaded)[0].items[0].fields[0].value == "New embedded" &&
                    (*loaded)[0].items[1].fields[3].value == "CUE title" &&
                    (*loaded)[0].items[0].source_revision == published_revision,
                "the durable source cache must dominate a stale debounced snapshot save");

        auto mismatched = refresh;
        mismatched.published_revision.size += 1U;
        const auto rejected = repository.refresh_local_metadata(mismatched);
        require(!rejected && rejected.error().code == core::ErrorCode::conflict,
                "one operation identity cannot be replayed with another revision");

        const core::LocalSourceRevision external_revision{
            .device = 100U,
            .inode = 200U,
            .size = 300U,
            .modification_time_seconds = 400,
            .modification_time_nanoseconds = 500,
        };
        auto externally_refreshed = stale_documents;
        for (auto& document : externally_refreshed) {
            for (auto& item : document.items) {
                if (item.source_reference != source) {
                    continue;
                }
                item.source_revision = external_revision;
                for (auto& snapshot_field : item.fields) {
                    if (snapshot_field.provenance == embedded && snapshot_field.name == "title") {
                        snapshot_field.value = "External title";
                    }
                }
            }
        }
        require(repository.replace_all(externally_refreshed).has_value(),
                "a freshly observed external source snapshot must persist");
        loaded = repository.load_all();
        require(loaded.has_value() && (*loaded)[0].items[0].fields[0].value == "External title" &&
                    (*loaded)[0].items[0].source_revision == external_revision,
                "a different freshly observed revision must not be shadowed by an old cache");

        auto latest = refresh;
        latest.operation_id = core::StableId::random();
        latest.previous_revision = external_revision;
        latest.published_revision = core::LocalSourceRevision{
            .device = 101U,
            .inode = 201U,
            .size = 301U,
            .modification_time_seconds = 401,
            .modification_time_nanoseconds = 501,
        };
        latest.document.fields[0].values = {"Latest title"};
        require(repository.refresh_local_metadata(latest) ==
                    persistence::LocalMetadataRefreshResult{.affected_occurrences = 3U,
                                                            .already_applied = false},
                "a later verified commit must replace the prior source cache");
    }
    {
        auto reopened = persistence::ListRepository::open(database_path);
        require(reopened.has_value(), "refreshed repository must reopen");
        auto loaded = reopened->load_all();
        require(loaded.has_value() && (*loaded)[0].items[0].fields[0].value == "Latest title" &&
                    (*loaded)[0].items[1].fields[2].value == "CUE title",
                "source cache and layered logical snapshots must survive restart");
    }
    cleanup();
}

void committed_source_relocation_rekeys_every_occurrence_and_stale_snapshot() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-source-relocation-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const std::string source = std::string{"/music/source-"} + '\xff' + ".flac";
    const std::string middle = std::string{"/music/middle-"} + '\xfe' + ".flac";
    const std::string target = std::string{"/archive/target-"} + '\xfd' + ".flac";
    const core::LocalSourceRevision observed{.device = 10,
                                             .inode = 20,
                                             .size = 30,
                                             .modification_time_seconds = 40,
                                             .modification_time_nanoseconds = 50};
    const core::LocalSourceRevision tagged{.device = 10,
                                           .inode = 20,
                                           .size = 31,
                                           .modification_time_seconds = 41,
                                           .modification_time_nanoseconds = 51};
    const core::LocalSourceRevision copied{.device = 11,
                                           .inode = 21,
                                           .size = 31,
                                           .modification_time_seconds = 41,
                                           .modification_time_nanoseconds = 51};
    const core::LocalSourceRevision copied_again{.device = 12,
                                                 .inode = 22,
                                                 .size = 31,
                                                 .modification_time_seconds = 41,
                                                 .modification_time_nanoseconds = 51};
    const auto profile_id = core::StableId::random();
    const auto embedded = metadata::FieldProvenance::embedded;
    const auto sidecar = metadata::FieldProvenance::sidecar;
    const std::vector<persistence::ListDocument> initial{
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "First",
            .pinned = false,
            .dirty = true,
            .items =
                {
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = source,
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = 1'000,
                        .source_revision = observed,
                        .fields = {{.name = "title",
                                    .value = "Before",
                                    .native_name = "TITLE",
                                    .provenance = embedded}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::local,
                        .profile_id = std::nullopt,
                        .source_reference = source,
                        .logical_reference = std::string{"cue-v1\0track-1", 14U},
                        .segment =
                            persistence::ListItemSegment{.start_sample = 0, .end_sample = 44'100},
                        .source_selection = std::nullopt,
                        .duration_ms = 1'000,
                        .source_revision = observed,
                        .fields = {{.name = "title",
                                    .value = "Before",
                                    .native_name = "TITLE",
                                    .provenance = embedded},
                                   {.name = "title",
                                    .value = "CUE title",
                                    .native_name = "TITLE",
                                    .provenance = sidecar}},
                    },
                    persistence::ListItem{
                        .source = persistence::ListSource::mpd,
                        .profile_id = profile_id,
                        .source_reference = middle,
                        .logical_reference = std::nullopt,
                        .segment = std::nullopt,
                        .source_selection = std::nullopt,
                        .duration_ms = 2'000,
                        .source_revision = std::nullopt,
                        .fields = {{"title", "Remote"}},
                    },
                },
        },
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = "Duplicate",
            .pinned = true,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = 1'000,
                .source_revision = observed,
                .fields = {{.name = "title",
                            .value = "Before",
                            .native_name = "TITLE",
                            .provenance = embedded}},
            }},
        },
    };

    auto opened = persistence::ListRepository::open(database_path);
    require(opened.has_value(), "source-relocation repository must open");
    auto repository = std::move(*opened);
    require(repository.replace_all(initial).has_value(), "source-relocation fixtures must persist");
    require(repository.refresh_local_metadata(persistence::LocalMetadataRefresh{
                .operation_id = core::StableId::random(),
                .source_reference = source,
                .previous_revision = observed,
                .published_revision = tagged,
                .document =
                    metadata::MetadataDocument{
                        .fields = {metadata::MetadataField{
                            .canonical_name = "title",
                            .native_name = "TITLE",
                            .values = {"After tagging"},
                            .qualifier = {},
                            .provenance = embedded,
                        }},
                        .unsupported_native_objects = {},
                    },
            }) == persistence::LocalMetadataRefreshResult{.affected_occurrences = 3U,
                                                          .already_applied = false},
            "relocation fixture must own a durable source cache");
    auto stale_snapshot = repository.load_all();
    require(stale_snapshot.has_value(), "pre-relocation snapshot must load");
    for (auto& document : *stale_snapshot) {
        for (auto& item : document.items) {
            if (item.source != persistence::ListSource::local || item.source_reference != source) {
                continue;
            }
            for (auto& field : item.fields) {
                if (field.provenance == embedded) {
                    field.value = "Stale workspace field";
                }
            }
        }
    }

    const auto first_id = core::StableId::random();
    const persistence::LocalSourceRelocation first{
        .operation_id = first_id,
        .source_reference = source,
        .target_reference = middle,
        .previous_revision = tagged,
        .published_revision = copied,
        .published_document =
            metadata::MetadataDocument{
                .fields = {metadata::MetadataField{
                    .canonical_name = "title",
                    .native_name = "TITLE",
                    .values = {"After combined publication"},
                    .qualifier = {},
                    .provenance = embedded,
                }},
                .unsupported_native_objects = {},
            },
    };
    require(repository.relocate_local_source(first) ==
                persistence::LocalSourceRelocationResult{.affected_occurrences = 3U,
                                                         .cache_rekeyed = true,
                                                         .metadata_refreshed = true,
                                                         .already_applied = false},
            "one transaction must re-key every duplicate and refresh the destination cache");
    auto loaded = repository.load_all();
    require(loaded.has_value(), "relocated documents must load");
    require((*loaded)[0].items[0].source_reference == middle &&
                (*loaded)[0].items[1].source_reference == middle &&
                (*loaded)[1].items[0].source_reference == middle &&
                (*loaded)[0].items[0].source_revision == copied &&
                (*loaded)[0].items[1].fields.back().value == "CUE title" &&
                (*loaded)[0].items[0].fields.front().value == "After combined publication" &&
                (*loaded)[0].items[2].source == persistence::ListSource::mpd &&
                (*loaded)[0].items[2].source_reference == middle,
            "relocation must preserve metadata overlays and leave MPD authority untouched");
    require(repository.relocate_local_source(first) ==
                persistence::LocalSourceRelocationResult{.affected_occurrences = 3U,
                                                         .cache_rekeyed = true,
                                                         .metadata_refreshed = true,
                                                         .already_applied = true},
            "recovery replay must be an idempotent no-op");
    auto missing_combined_document = first;
    missing_combined_document.published_document.reset();
    const auto rejected_content_replay =
        repository.relocate_local_source(missing_combined_document);
    require(!rejected_content_replay &&
                rejected_content_replay.error().code == core::ErrorCode::conflict,
            "combined relocation evidence cannot replay as a path-only transaction");
    auto mismatched = first;
    mismatched.target_reference = target;
    const auto rejected_replay = repository.relocate_local_source(mismatched);
    require(!rejected_replay && rejected_replay.error().code == core::ErrorCode::conflict,
            "one relocation identity cannot be replayed with another target");

    require(repository.replace_all(*stale_snapshot).has_value(),
            "a delayed pre-relocation workspace snapshot may still arrive");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == middle &&
                (*loaded)[0].items[0].source_revision == copied &&
                (*loaded)[0].items[0].fields.front().value == "After combined publication",
            "relocation history and the refreshed cache must dominate a stale snapshot");

    const persistence::LocalSourceRelocation second{
        .operation_id = core::StableId::random(),
        .source_reference = middle,
        .target_reference = target,
        .previous_revision = copied,
        .published_revision = copied_again,
        .published_document = std::nullopt,
    };
    require(repository.relocate_local_source(second) ==
                persistence::LocalSourceRelocationResult{.affected_occurrences = 3U,
                                                         .cache_rekeyed = true,
                                                         .metadata_refreshed = false,
                                                         .already_applied = false},
            "a later relocation must advance the same physical source again");
    require(repository.replace_all(*stale_snapshot).has_value(),
            "a snapshot from before both relocations may still be submitted");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == target &&
                (*loaded)[0].items[1].source_reference == target &&
                (*loaded)[1].items[0].source_reference == target &&
                (*loaded)[0].items[0].source_revision == copied_again &&
                (*loaded)[0].items[0].fields.front().value == "After combined publication",
            "relocation replay must follow the ordered source-to-target chain");

    const core::LocalSourceRevision reused_path_revision{.device = 90,
                                                         .inode = 91,
                                                         .size = 92,
                                                         .modification_time_seconds = 93,
                                                         .modification_time_nanoseconds = 94};
    auto path_reused = *loaded;
    path_reused.front().items.push_back(persistence::ListItem{
        .source = persistence::ListSource::local,
        .profile_id = std::nullopt,
        .source_reference = source,
        .logical_reference = std::nullopt,
        .segment = std::nullopt,
        .source_selection = std::nullopt,
        .duration_ms = std::nullopt,
        .source_revision = reused_path_revision,
        .fields = {{.name = "title",
                    .value = "New file at old path",
                    .native_name = "TITLE",
                    .provenance = embedded}},
    });
    require(repository.replace_all(path_reused).has_value(),
            "a different physical revision may later reuse the old raw path");
    loaded = repository.load_all();
    require(loaded && loaded->front().items.back().source_reference == source &&
                loaded->front().items.back().source_revision == reused_path_revision,
            "revision-qualified history must not redirect a different file at a reused path");
    const auto collision = repository.relocate_local_source(persistence::LocalSourceRelocation{
        .operation_id = core::StableId::random(),
        .source_reference = source,
        .target_reference = target,
        .previous_revision = reused_path_revision,
        .published_revision = reused_path_revision,
        .published_document = std::nullopt,
    });
    require(!collision && collision.error().code == core::ErrorCode::conflict &&
                repository.load_all() == loaded,
            "a persisted target collision must reject the complete relocation transaction");
    auto reopened = persistence::ListRepository::open(database_path);
    require(reopened && reopened->schema_version() == 31U && reopened->load_all() == loaded,
            "relocation evidence and resolved paths must survive reopening schema 18");

    cleanup();
}

void unowned_target_metadata_cache_is_superseded_by_relocation() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path = std::filesystem::temp_directory_path() /
                               ("trackknife-reused-relocation-target-" +
                                core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    const std::string old_source{"/music/old-source.flac"};
    const std::string current_source{"/music/current-source.flac"};
    const std::string reused_target{"/music/reused-target.flac"};
    const core::LocalSourceRevision old_revision{.device = 1,
                                                 .inode = 2,
                                                 .size = 3,
                                                 .modification_time_seconds = 4,
                                                 .modification_time_nanoseconds = 5};
    const core::LocalSourceRevision old_tagged{.device = 1,
                                               .inode = 6,
                                               .size = 7,
                                               .modification_time_seconds = 8,
                                               .modification_time_nanoseconds = 9};
    const core::LocalSourceRevision old_target_revision{.device = 1,
                                                        .inode = 6,
                                                        .size = 7,
                                                        .modification_time_seconds = 10,
                                                        .modification_time_nanoseconds = 11};
    const core::LocalSourceRevision current_revision{.device = 1,
                                                     .inode = 12,
                                                     .size = 13,
                                                     .modification_time_seconds = 14,
                                                     .modification_time_nanoseconds = 15};
    const core::LocalSourceRevision published_revision{.device = 1,
                                                       .inode = 16,
                                                       .size = 17,
                                                       .modification_time_seconds = 18,
                                                       .modification_time_nanoseconds = 19};
    const auto document_id = core::StableId::random();
    const auto list_document = [&](const std::string& source,
                                   const core::LocalSourceRevision& revision,
                                   const std::string& title) {
        return persistence::ListDocument{
            .id = document_id,
            .kind = persistence::ListKind::scratch,
            .name = "Reused target",
            .pinned = false,
            .dirty = true,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = revision,
                .fields = {{.name = "title",
                            .value = title,
                            .native_name = "TITLE",
                            .provenance = metadata::FieldProvenance::embedded}},
            }},
        };
    };
    const auto metadata_document = [](const std::string& title) {
        return metadata::MetadataDocument{
            .fields = {metadata::MetadataField{
                .canonical_name = "title",
                .native_name = "TITLE",
                .values = {title},
                .qualifier = {},
                .provenance = metadata::FieldProvenance::embedded,
            }},
            .unsupported_native_objects = {},
        };
    };

    auto opened = persistence::ListRepository::open(database_path);
    require(opened.has_value(), "reused-target repository must open");
    auto repository = std::move(*opened);
    const std::vector old_documents{list_document(old_source, old_revision, "Old title")};
    require(repository.replace_all(old_documents).has_value(),
            "old target owner must persist before the first publication");
    require(repository
                .refresh_local_metadata(persistence::LocalMetadataRefresh{
                    .operation_id = core::StableId::random(),
                    .source_reference = old_source,
                    .previous_revision = old_revision,
                    .published_revision = old_tagged,
                    .document = metadata_document("Old cached title"),
                })
                .has_value(),
            "the first publication must establish a source metadata cache");
    require(repository
                .relocate_local_source(persistence::LocalSourceRelocation{
                    .operation_id = core::StableId::random(),
                    .source_reference = old_source,
                    .target_reference = reused_target,
                    .previous_revision = old_tagged,
                    .published_revision = old_target_revision,
                    .published_document = std::nullopt,
                })
                .has_value(),
            "the first path publication must establish the historical target cache");

    const std::vector current_documents{
        list_document(current_source, current_revision, "Current source title")};
    require(repository.replace_all(current_documents).has_value(),
            "the historical target cache may outlive its last list occurrence");
    const auto combined = repository.relocate_local_source(persistence::LocalSourceRelocation{
        .operation_id = core::StableId::random(),
        .source_reference = current_source,
        .target_reference = reused_target,
        .previous_revision = current_revision,
        .published_revision = published_revision,
        .published_document = metadata_document("New published title"),
    });
    require(combined == persistence::LocalSourceRelocationResult{.affected_occurrences = 1U,
                                                                 .cache_rekeyed = false,
                                                                 .metadata_refreshed = true,
                                                                 .already_applied = false},
            "an unowned historical target cache must be atomically superseded");
    auto loaded = repository.load_all();
    require(loaded && loaded->front().items.front().source_reference == reused_target &&
                loaded->front().items.front().source_revision == published_revision &&
                loaded->front().items.front().fields.front().value == "New published title",
            "the reused target must expose only its newly verified revision and metadata");
    require(repository.replace_all(current_documents).has_value(),
            "a delayed current-source snapshot must still follow the new relocation");
    loaded = repository.load_all();
    require(loaded && loaded->front().items.front().source_reference == reused_target &&
                loaded->front().items.front().source_revision == published_revision &&
                loaded->front().items.front().fields.front().value == "New published title",
            "the replacement cache must continue to dominate a delayed workspace snapshot");
    cleanup();
}

void previously_resolved_target_reconciles_fresh_relocation() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-pre-resolved-relocation-" + core::StableId::random().to_string() + ".sqlite3");
    const auto cleanup = [&database_path] {
        std::error_code ignored;
        std::filesystem::remove(database_path, ignored);
        std::filesystem::remove(database_path.string() + "-wal", ignored);
        std::filesystem::remove(database_path.string() + "-shm", ignored);
    };
    cleanup();

    const std::string source{"/music/source.flac"};
    const std::string target{"/music/organized/source.flac"};
    const core::LocalSourceRevision source_revision{.device = 10,
                                                    .inode = 11,
                                                    .size = 12,
                                                    .modification_time_seconds = 13,
                                                    .modification_time_nanoseconds = 14};
    const core::LocalSourceRevision published_revision{.device = 10,
                                                       .inode = 11,
                                                       .size = 20,
                                                       .modification_time_seconds = 21,
                                                       .modification_time_nanoseconds = 22};
    const auto document_id = core::StableId::random();
    const auto list_document = [&](const std::string& reference, const std::string& title) {
        return persistence::ListDocument{
            .id = document_id,
            .kind = persistence::ListKind::scratch,
            .name = "Freshly reopened source",
            .pinned = false,
            .dirty = true,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = reference,
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_revision,
                .fields = {{.name = "title",
                            .value = title,
                            .native_name = "TITLE",
                            .provenance = metadata::FieldProvenance::embedded}},
            }},
        };
    };
    const auto published_document = metadata::MetadataDocument{
        .fields = {metadata::MetadataField{
            .canonical_name = "title",
            .native_name = "TITLE",
            .values = {"Published again"},
            .qualifier = {},
            .provenance = metadata::FieldProvenance::embedded,
        }},
        .unsupported_native_objects = {},
    };

    auto opened = persistence::ListRepository::open(database_path);
    require(opened.has_value(), "pre-resolved relocation repository must open");
    auto repository = std::move(*opened);
    const std::vector original{list_document(source, "Original")};
    require(repository.replace_all(original).has_value(), "original source must persist");
    require(repository
                .relocate_local_source(persistence::LocalSourceRelocation{
                    .operation_id = core::StableId::random(),
                    .source_reference = source,
                    .target_reference = target,
                    .previous_revision = source_revision,
                    .published_revision = source_revision,
                    .published_document = std::nullopt,
                })
                .has_value(),
            "the first relocation must establish revision-qualified history");

    const std::vector reopened{list_document(source, "Freshly reopened")};
    require(repository.replace_all(reopened).has_value(),
            "workspace persistence may pre-resolve a reopened source to its old target");
    auto loaded = repository.load_all();
    require(loaded && loaded->front().items.front().source_reference == target &&
                loaded->front().items.front().source_revision == source_revision,
            "the regression fixture must begin with the occurrence already at the target");

    const auto operation_id = core::StableId::random();
    const persistence::LocalSourceRelocation relocation{
        .operation_id = operation_id,
        .source_reference = source,
        .target_reference = target,
        .previous_revision = source_revision,
        .published_revision = published_revision,
        .published_document = published_document,
    };
    require(repository.relocate_local_source(relocation) ==
                persistence::LocalSourceRelocationResult{.affected_occurrences = 1U,
                                                         .cache_rekeyed = false,
                                                         .metadata_refreshed = true,
                                                         .already_applied = false},
            "an exact pre-resolved target occurrence must join the fresh publication");
    loaded = repository.load_all();
    require(loaded && loaded->front().items.front().source_reference == target &&
                loaded->front().items.front().source_revision == published_revision &&
                loaded->front().items.front().fields.front().value == "Published again",
            "pre-resolved occurrences must receive the published revision and metadata");
    require(repository.relocate_local_source(relocation) ==
                persistence::LocalSourceRelocationResult{.affected_occurrences = 1U,
                                                         .cache_rekeyed = false,
                                                         .metadata_refreshed = true,
                                                         .already_applied = true},
            "pre-resolved reconciliation must remain idempotent");
    cleanup();
}

void legacy_logical_snapshots_block_refresh() {
    namespace core = trackknife::core;
    namespace metadata = trackknife::metadata;
    namespace persistence = trackknife::persistence;
    const auto database_path =
        std::filesystem::temp_directory_path() /
        ("trackknife-legacy-refresh-" + core::StableId::random().to_string() + ".sqlite3");
    const std::string source{"/music/legacy.flac"};
    auto opened = persistence::ListRepository::open(database_path);
    require(opened.has_value(), "legacy refresh repository must open");
    auto repository = std::move(*opened);
    const std::vector documents{persistence::ListDocument{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::scratch,
        .name = "Legacy",
        .pinned = false,
        .dirty = false,
        .items = {persistence::ListItem{
            .source = persistence::ListSource::local,
            .profile_id = std::nullopt,
            .source_reference = source,
            .logical_reference = std::string{"cue-v1\0legacy", 13U},
            .segment = persistence::ListItemSegment{.start_sample = 0, .end_sample = 1},
            .source_selection = std::nullopt,
            .duration_ms = std::nullopt,
            .source_revision = std::nullopt,
            .fields = {{"title", "Flattened CUE title"}},
        }},
    }};
    require(repository.replace_all(documents).has_value(), "legacy snapshot must persist");
    const auto rejected = repository.refresh_local_metadata(persistence::LocalMetadataRefresh{
        .operation_id = core::StableId::random(),
        .source_reference = source,
        .previous_revision = core::LocalSourceRevision{.device = 6,
                                                       .inode = 7,
                                                       .size = 8,
                                                       .modification_time_seconds = 9,
                                                       .modification_time_nanoseconds = 10},
        .published_revision = core::LocalSourceRevision{.device = 1,
                                                        .inode = 2,
                                                        .size = 3,
                                                        .modification_time_seconds = 4,
                                                        .modification_time_nanoseconds = 5},
        .document =
            metadata::MetadataDocument{.fields = {metadata::MetadataField{
                                           .canonical_name = "title",
                                           .native_name = "TITLE",
                                           .values = {"New"},
                                           .qualifier = {},
                                           .provenance = metadata::FieldProvenance::embedded}},
                                       .unsupported_native_objects = {}},
    });
    require(!rejected && rejected.error().code == core::ErrorCode::conflict &&
                repository.load_all() == documents,
            "flattened logical snapshots must block instead of losing their overlay");
    std::error_code ignored;
    std::filesystem::remove(database_path, ignored);
    std::filesystem::remove(database_path.string() + "-wal", ignored);
    std::filesystem::remove(database_path.string() + "-shm", ignored);
}

} // namespace

int main() {
    list_documents_round_trip_transactionally();
    metadata_transformation_chains_round_trip_transactionally();
    output_layout_and_destination_profiles_round_trip_transactionally();
    encoder_presets_round_trip_transactionally();
    committed_metadata_refreshes_every_occurrence_idempotently();
    committed_source_relocation_rekeys_every_occurrence_and_stale_snapshot();
    unowned_target_metadata_cache_is_superseded_by_relocation();
    previously_resolved_target_reconciles_fresh_relocation();
    legacy_logical_snapshots_block_refresh();
    return EXIT_SUCCESS;
}
