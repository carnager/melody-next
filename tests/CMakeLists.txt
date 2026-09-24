# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_executable(trackknife_core_tests core_primitives_test.cpp)
target_link_libraries(
    trackknife_core_tests
    PRIVATE Trackknife::Core trackknife_project_options trackknife_project_warnings
)
add_test(NAME core-primitives COMMAND trackknife_core_tests)

add_executable(trackknife_metadata_reader_tests metadata_reader_test.cpp)
target_link_libraries(
    trackknife_metadata_reader_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME metadata-reader
    COMMAND trackknife_metadata_reader_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_artwork_tests metadata_artwork_test.cpp)
target_link_libraries(
    trackknife_metadata_artwork_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-artwork
    COMMAND trackknife_metadata_artwork_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_media_streams_tests media_streams_test.cpp)
target_link_libraries(
    trackknife_media_streams_tests
    PRIVATE Trackknife::Engine trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME media-streams
    COMMAND trackknife_media_streams_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_cover_fitting_tests cover_fitting_test.cpp)
target_link_libraries(
    trackknife_cover_fitting_tests
    PRIVATE Trackknife::Engine trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME cover-fitting
    COMMAND trackknife_cover_fitting_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_artwork_export_tests artwork_export_test.cpp)
target_link_libraries(
    trackknife_artwork_export_tests
    PRIVATE Trackknife::Operations Trackknife::Formats Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME artwork-export
    COMMAND trackknife_artwork_export_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_selection_tests metadata_selection_test.cpp)
target_link_libraries(
    trackknife_metadata_selection_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-selection COMMAND trackknife_metadata_selection_tests)

add_executable(trackknife_metadata_proposal_tests metadata_proposal_test.cpp)
target_link_libraries(
    trackknife_metadata_proposal_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-proposal COMMAND trackknife_metadata_proposal_tests)

add_executable(trackknife_metadata_patch_tests metadata_patch_test.cpp)
target_link_libraries(
    trackknife_metadata_patch_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-patch COMMAND trackknife_metadata_patch_tests)

add_executable(trackknife_metadata_loudness_sidecar_tests metadata_loudness_sidecar_test.cpp)
target_link_libraries(
    trackknife_metadata_loudness_sidecar_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-loudness-sidecar COMMAND trackknife_metadata_loudness_sidecar_tests)

add_executable(trackknife_query_tkq_tests query_tkq_test.cpp)
target_link_libraries(
    trackknife_query_tkq_tests
    PRIVATE Trackknife::Query trackknife_project_options trackknife_project_warnings
)
add_test(NAME query-tkq COMMAND trackknife_query_tkq_tests)

add_executable(trackknife_local_library_filter_tests local_library_filter_test.cpp)
target_link_libraries(
    trackknife_local_library_filter_tests
    PRIVATE Trackknife::Persistence Trackknife::Query PkgConfig::TAGLIB PkgConfig::SQLITE3
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME local-library-filter COMMAND trackknife_local_library_filter_tests
         "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")
set_tests_properties(local-library-filter PROPERTIES TIMEOUT 60 ENVIRONMENT
    "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

add_executable(trackknife_metadata_transformation_tests metadata_transformation_test.cpp)
target_link_libraries(
    trackknife_metadata_transformation_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-transformation COMMAND trackknife_metadata_transformation_tests)

add_executable(trackknife_metadata_write_plan_tests metadata_write_plan_test.cpp)
target_link_libraries(
    trackknife_metadata_write_plan_tests
    PRIVATE Trackknife::Metadata trackknife_project_options trackknife_project_warnings
)
add_test(NAME metadata-write-plan COMMAND trackknife_metadata_write_plan_tests)

add_executable(trackknife_metadata_flac_writer_tests metadata_flac_writer_test.cpp)
target_link_libraries(
    trackknife_metadata_flac_writer_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-flac-writer
    COMMAND trackknife_metadata_flac_writer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_loudness_analyzer_tests loudness_analyzer_test.cpp)
target_link_libraries(
    trackknife_loudness_analyzer_tests
    PRIVATE Trackknife::Loudness Trackknife::Formats Trackknife::Metadata trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME loudness-analyzer
    COMMAND trackknife_loudness_analyzer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_audio_convert_tests audio_convert_test.cpp)
target_link_libraries(
    trackknife_audio_convert_tests
    PRIVATE Trackknife::Convert Trackknife::Formats Trackknife::Metadata PkgConfig::TAGLIB
            trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME audio-convert
    COMMAND trackknife_audio_convert_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_wavpack_writer_tests metadata_wavpack_writer_test.cpp)
target_link_libraries(
    trackknife_metadata_wavpack_writer_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-wavpack-writer
    COMMAND trackknife_metadata_wavpack_writer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_ogg_writer_tests metadata_ogg_writer_test.cpp)
target_link_libraries(
    trackknife_metadata_ogg_writer_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-ogg-writer
    COMMAND trackknife_metadata_ogg_writer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_mp4_writer_tests metadata_mp4_writer_test.cpp)
target_link_libraries(
    trackknife_metadata_mp4_writer_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-mp4-writer
    COMMAND trackknife_metadata_mp4_writer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_metadata_mp3_writer_tests metadata_mp3_writer_test.cpp)
target_link_libraries(
    trackknife_metadata_mp3_writer_tests
    PRIVATE Trackknife::Metadata Trackknife::Formats PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME metadata-mp3-writer
    COMMAND trackknife_metadata_mp3_writer_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_list_repository_tests list_repository_test.cpp)
target_link_libraries(
    trackknife_list_repository_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME list-repository COMMAND trackknife_list_repository_tests)

add_executable(trackknife_musicbrainz_cache_tests musicbrainz_cache_test.cpp)
target_link_libraries(
    trackknife_musicbrainz_cache_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME musicbrainz-cache COMMAND trackknife_musicbrainz_cache_tests)

add_executable(trackknife_workspace_backup_tests workspace_backup_test.cpp)
target_link_libraries(
    trackknife_workspace_backup_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME workspace-backup COMMAND trackknife_workspace_backup_tests)

add_executable(trackknife_operation_journal_tests operation_journal_test.cpp)
target_link_libraries(
    trackknife_operation_journal_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME operation-journal COMMAND trackknife_operation_journal_tests)

add_executable(trackknife_file_publication_journal_tests file_publication_journal_test.cpp)
target_link_libraries(
    trackknife_file_publication_journal_tests
    PRIVATE Trackknife::Persistence trackknife_project_options trackknife_project_warnings
)
add_test(NAME file-publication-journal COMMAND trackknife_file_publication_journal_tests)

add_executable(trackknife_file_publication_tests file_publication_test.cpp)
target_link_libraries(
    trackknife_file_publication_tests
    PRIVATE Trackknife::Operations Trackknife::Persistence trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME file-publication COMMAND trackknife_file_publication_tests)
set_tests_properties(file-publication PROPERTIES TIMEOUT 30)

add_executable(trackknife_file_publication_apply_tests file_publication_apply_test.cpp)
target_link_libraries(
    trackknife_file_publication_apply_tests
    PRIVATE Trackknife::Operations Trackknife::Persistence Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME file-publication-apply COMMAND trackknife_file_publication_apply_tests)
set_tests_properties(file-publication-apply PROPERTIES TIMEOUT 30)

add_executable(trackknife_metadata_commit_tests metadata_commit_test.cpp)
target_link_libraries(
    trackknife_metadata_commit_tests
    PRIVATE Trackknife::Operations Trackknife::Persistence Threads::Threads PkgConfig::TAGLIB
            trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME metadata-commit
    COMMAND trackknife_metadata_commit_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)
set_tests_properties(metadata-commit PROPERTIES TIMEOUT 30)

add_executable(trackknife_output_path_plan_tests output_path_plan_test.cpp)
target_link_libraries(
    trackknife_output_path_plan_tests
    PRIVATE Trackknife::Operations trackknife_project_options trackknife_project_warnings
)
add_test(NAME output-path-plan COMMAND trackknife_output_path_plan_tests)

add_executable(trackknife_output_path_preflight_tests output_path_preflight_test.cpp)
target_link_libraries(
    trackknife_output_path_preflight_tests
    PRIVATE Trackknife::Operations trackknife_project_options trackknife_project_warnings
)
add_test(NAME output-path-preflight COMMAND trackknife_output_path_preflight_tests)

add_executable(trackknife_preparation_plan_tests preparation_plan_test.cpp)
target_link_libraries(
    trackknife_preparation_plan_tests
    PRIVATE Trackknife::Operations trackknife_project_options trackknife_project_warnings
)
add_test(NAME preparation-plan COMMAND trackknife_preparation_plan_tests)

add_executable(trackknife_formats_probe_tests formats_probe_test.cpp)
target_link_libraries(
    trackknife_formats_probe_tests
    PRIVATE Trackknife::Formats trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME formats-probe
    COMMAND trackknife_formats_probe_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_cue_sheet_tests cue_sheet_test.cpp)
target_link_libraries(
    trackknife_cue_sheet_tests
    PRIVATE Trackknife::Formats trackknife_project_options trackknife_project_warnings
)
add_test(NAME cue-sheet COMMAND trackknife_cue_sheet_tests)

# ADR-0220 Phase 0: links Trackknife::Audio only -- no Qt, no widgets -- which
# is the property this suite exists to hold.
# ADR-0222: the protocol v1 golden corpus. Links no Qt; the same corpus file is
# meant to be run by the Go implementation.
add_executable(trackknife_protocol_message_tests protocol_message_test.cpp)
target_link_libraries(
    trackknife_protocol_message_tests
    PRIVATE Trackknife::Protocol Trackknife::Core trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME protocol-message
    COMMAND trackknife_protocol_message_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/protocol/protocol-v1-corpus.json"
)

# ADR-0222: the catalogue methods against a real database, over the wire
# format. Links no Qt.
# ADR-0219/0222: jobs, including the fault injection proving a long job cannot
# starve the control path.
# ADR-0220: the same catalogue interface, local and remote, answering the same.
add_executable(trackknife_remote_catalogue_tests remote_catalogue_test.cpp)
target_link_libraries(
    trackknife_remote_catalogue_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME remote-catalogue COMMAND trackknife_remote_catalogue_tests)

# ADR-0222: the real client against the real server.
add_executable(trackknife_protocol_roundtrip_tests protocol_roundtrip_test.cpp)
target_link_libraries(
    trackknife_protocol_roundtrip_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME protocol-roundtrip COMMAND trackknife_protocol_roundtrip_tests)
set_tests_properties(protocol-roundtrip PROPERTIES TIMEOUT 60)

# ADR-0220 Phase 2: playback owned by the engine.
add_executable(trackknife_engine_player_tests engine_player_test.cpp)
target_link_libraries(
    trackknife_engine_player_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME engine-player
    COMMAND trackknife_engine_player_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

# ADR-0228: an engine playing on an output agent, both real, in one process.
add_executable(trackknife_engine_lastfm_tests engine_lastfm_test.cpp)
target_link_libraries(trackknife_engine_lastfm_tests PRIVATE Trackknife::Engine
    trackknife_project_options trackknife_project_warnings)
add_test(NAME engine-lastfm COMMAND trackknife_engine_lastfm_tests)
set_tests_properties(engine-lastfm PROPERTIES TIMEOUT 60)

add_executable(trackknife_output_agent_tests output_agent_test.cpp)
target_link_libraries(
    trackknife_output_agent_tests
    PRIVATE Trackknife::Agent Trackknife::Engine Trackknife::Protocol Trackknife::Core
            Threads::Threads PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
target_compile_definitions(
    trackknife_output_agent_tests
    PRIVATE TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)
add_test(
    NAME output-agent
    COMMAND trackknife_output_agent_tests
)
set_tests_properties(output-agent PROPERTIES TIMEOUT 90)

# ADR-0220: melodyd as a process, exercised the way an init system would.
add_test(
    NAME engine-daemon
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/engine_daemon_test.sh"
            "$<TARGET_FILE:trackknife_engine_daemon>"
)
set_tests_properties(engine-daemon PROPERTIES TIMEOUT 60)

add_test(
    NAME engine-discovery
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/engine_discovery_test.sh"
            "$<TARGET_FILE:trackknife_engine_daemon>" "$<TARGET_FILE:melody_agent>"
)
set_tests_properties(engine-discovery PROPERTIES TIMEOUT 60)

add_test(
    NAME melody-cli
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cli_test.sh"
            "$<TARGET_FILE:trackknife_engine_daemon>" "$<TARGET_FILE:melody_cli>"
)
set_tests_properties(melody-cli PROPERTIES TIMEOUT 90)

if(TARGET trackknife_bench_ui)
    qt_add_executable(trackknife_remote_mount_tests remote_mount_test.cpp)
    target_link_libraries(trackknife_remote_mount_tests PRIVATE Trackknife::BenchUi Qt6::Test
        trackknife_project_options trackknife_project_warnings)
    add_test(NAME remote-mount COMMAND trackknife_remote_mount_tests)
    qt_add_executable(trackknife_engine_launcher_tests engine_launcher_test.cpp)
    target_link_libraries(
        trackknife_engine_launcher_tests
        PRIVATE Trackknife::BenchUi Qt6::Network Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    target_compile_definitions(
        trackknife_engine_launcher_tests
        PRIVATE TRACKKNIFE_ENGINE_BINARY="$<TARGET_FILE:trackknife_engine_daemon>"
    )
    add_dependencies(trackknife_engine_launcher_tests trackknife_engine_daemon)
    add_test(NAME engine-launcher COMMAND trackknife_engine_launcher_tests)
    set_tests_properties(engine-launcher PROPERTIES TIMEOUT 60)
endif()

# ADR-0222: the unix socket, talked to with plain POSIX calls and plain lines.
add_executable(trackknife_engine_server_tests engine_server_test.cpp)
target_link_libraries(
    trackknife_engine_server_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME engine-server COMMAND trackknife_engine_server_tests)

add_executable(trackknife_job_registry_tests job_registry_test.cpp)
target_link_libraries(
    trackknife_job_registry_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME job-registry COMMAND trackknife_job_registry_tests)

add_executable(trackknife_engine_methods_tests engine_methods_test.cpp)
target_link_libraries(
    trackknife_engine_methods_tests
    PRIVATE Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME engine-methods COMMAND trackknife_engine_methods_tests
                                 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")

add_executable(trackknife_playback_state_tests playback_state_test.cpp)
target_link_libraries(
    trackknife_playback_state_tests
    PRIVATE Trackknife::Audio trackknife_project_options trackknife_project_warnings
)
add_test(NAME playback-state COMMAND trackknife_playback_state_tests)

add_executable(trackknife_local_playback_tests local_playback_test.cpp)
target_link_libraries(
    trackknife_local_playback_tests
    PRIVATE Trackknife::Audio Threads::Threads PkgConfig::TAGLIB trackknife_project_options
            trackknife_project_warnings
)
add_test(
    NAME local-playback
    COMMAND trackknife_local_playback_tests "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
)

add_executable(trackknife_local_audition_tests local_audition_test.cpp)
target_link_libraries(
    trackknife_local_audition_tests
    PRIVATE Trackknife::Audio Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME local-audition COMMAND trackknife_local_audition_tests)

set_tests_properties(
    local-audition
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 15
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
)

add_executable(trackknife_pipewire_output_tests pipewire_output_test.cpp)
target_link_libraries(
    trackknife_pipewire_output_tests
    PRIVATE Trackknife::Audio Threads::Threads trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME pipewire-output COMMAND trackknife_pipewire_output_tests)
set_tests_properties(
    pipewire-output
    PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 10
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
)

add_executable(trackknife_titleformat_parser_tests titleformat_parser_test.cpp)
target_link_libraries(
    trackknife_titleformat_parser_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-parser COMMAND trackknife_titleformat_parser_tests)

add_executable(trackknife_titleformat_compiler_tests titleformat_compiler_test.cpp)
target_link_libraries(
    trackknife_titleformat_compiler_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-compiler COMMAND trackknife_titleformat_compiler_tests)

add_executable(trackknife_titleformat_evaluator_tests titleformat_evaluator_test.cpp)
target_link_libraries(
    trackknife_titleformat_evaluator_tests
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-evaluator COMMAND trackknife_titleformat_evaluator_tests)

find_package(nlohmann_json 3.11 CONFIG REQUIRED)
add_executable(trackknife_titleformat_corpus_runner titleformat_corpus_runner.cpp)
target_link_libraries(
    trackknife_titleformat_corpus_runner
    PRIVATE Trackknife::TitleFormat nlohmann_json::nlohmann_json
            trackknife_project_options trackknife_project_warnings
)
add_test(
    NAME titleformat-corpus
    COMMAND trackknife_titleformat_corpus_runner
            "${CMAKE_CURRENT_SOURCE_DIR}/titleformat/tkfmt-corpus"
)

add_test(
    NAME titleformat-cli-smoke
    COMMAND trackknife_titleformat_cli --field artist=Trackknife --show-dependencies
            "%artist%"
)

add_test(
    NAME titleformat-cli-metadata-smoke
    COMMAND trackknife_titleformat_cli --field artist=One --field artist=Two --info channels=2
            "$join(artist, + )|$info(channels)"
)

if(TRACKKNIFE_BUILD_UI)
    qt_add_executable(trackknife_musicbrainz_web_service_tests musicbrainz_web_service_test.cpp)
    target_link_libraries(
        trackknife_musicbrainz_web_service_tests
        PRIVATE Trackknife::MusicBrainz trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME musicbrainz-web-service COMMAND trackknife_musicbrainz_web_service_tests)

    qt_add_executable(trackknife_musicbrainz_matching_tests musicbrainz_matching_test.cpp)
    target_link_libraries(
        trackknife_musicbrainz_matching_tests
        PRIVATE Trackknife::MusicBrainz trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME musicbrainz-matching COMMAND trackknife_musicbrainz_matching_tests)

    qt_add_executable(trackknife_musicbrainz_client_tests musicbrainz_client_test.cpp)
    target_link_libraries(
        trackknife_musicbrainz_client_tests
        PRIVATE Trackknife::MusicBrainz Qt6::Test Qt6::Network trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME musicbrainz-client COMMAND trackknife_musicbrainz_client_tests)

    qt_add_executable(
        trackknife_metadata_transformation_interchange_tests
        metadata_transformation_interchange_test.cpp
    )
    target_link_libraries(
        trackknife_metadata_transformation_interchange_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(
        NAME metadata-transformation-interchange
        COMMAND trackknife_metadata_transformation_interchange_tests
    )

    qt_add_executable(trackknife_panel_layout_tests panel_layout_test.cpp)
    target_link_libraries(
        trackknife_panel_layout_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME panel-layout COMMAND trackknife_panel_layout_tests)

    qt_add_executable(trackknife_track_view_layout_tests track_view_layout_test.cpp)
    target_link_libraries(
        trackknife_track_view_layout_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME track-view-layout COMMAND trackknife_track_view_layout_tests)

    qt_add_executable(trackknife_queue_table_view_tests queue_table_view_test.cpp)
    target_link_libraries(
        trackknife_queue_table_view_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME queue-table-view COMMAND trackknife_queue_table_view_tests)
    set_tests_properties(queue-table-view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_local_artwork_tests local_artwork_test.cpp)
    target_link_libraries(
        trackknife_local_artwork_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME local-artwork COMMAND trackknife_local_artwork_tests)
    set_tests_properties(local-artwork PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_artwork_fitting_tests artwork_fitting_test.cpp)
    target_link_libraries(
        trackknife_artwork_fitting_tests
        PRIVATE Trackknife::BenchUi Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME artwork-fitting COMMAND trackknife_artwork_fitting_tests)
    set_tests_properties(artwork-fitting PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(trackknife_local_folder_tree_model_tests local_folder_tree_model_test.cpp)
    target_link_libraries(
        trackknife_local_folder_tree_model_tests
        PRIVATE Trackknife::UiCommon Qt6::Test trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME local-folder-tree-model COMMAND trackknife_local_folder_tree_model_tests)
    set_tests_properties(local-folder-tree-model PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

        qt_add_executable(trackknife_lastfm_service_tests lastfm_service_test.cpp)
    target_link_libraries(trackknife_lastfm_service_tests PRIVATE Trackknife::BenchUi Qt6::Network
        Qt6::Test trackknife_project_options trackknife_project_warnings)
    add_test(NAME lastfm-service COMMAND trackknife_lastfm_service_tests)
    set_tests_properties(lastfm-service PROPERTIES TIMEOUT 30)
    qt_add_executable(trackknife_bench_main_window_tests bench_main_window_test.cpp)
    qt_add_executable(trackknife_mpris_service_tests mpris_service_test.cpp)
    target_link_libraries(trackknife_mpris_service_tests PRIVATE Trackknife::BenchUi Qt6::DBus
        Qt6::Test trackknife_project_options trackknife_project_warnings)
    add_test(NAME mpris-service COMMAND trackknife_mpris_service_tests)
    set_tests_properties(mpris-service PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(trackknife_local_library_tests local_library_test.cpp)
    target_compile_definitions(trackknife_local_library_tests PRIVATE
        TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
        TRACKKNIFE_MIGRATION_DIR="${PROJECT_SOURCE_DIR}/src/persistence/migrations"
        TRACKKNIFE_ENGINE_BINARY="$<TARGET_FILE:trackknife_engine_daemon>")
    add_dependencies(trackknife_local_library_tests trackknife_engine_daemon)
    target_link_libraries(trackknife_local_library_tests PRIVATE Trackknife::BenchUi
        Qt6::Test PkgConfig::TAGLIB PkgConfig::SQLITE3 trackknife_project_options trackknife_project_warnings)
    add_test(NAME local-library COMMAND trackknife_local_library_tests)
    set_tests_properties(local-library PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
    target_compile_definitions(
        trackknife_bench_main_window_tests
        PRIVATE TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio"
                TRACKKNIFE_ENGINE_BINARY="$<TARGET_FILE:trackknife_engine_daemon>"
                TRACKKNIFE_AGENT_BINARY="$<TARGET_FILE:melody_agent>"
    )
    add_dependencies(trackknife_bench_main_window_tests trackknife_engine_daemon melody_agent)
    target_link_libraries(
        trackknife_bench_main_window_tests
        PRIVATE Trackknife::BenchUi Qt6::Network Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME bench-main-window COMMAND trackknife_bench_main_window_tests)
    qt_add_executable(trackknife_m3u8_tests m3u8_test.cpp)
    target_link_libraries(trackknife_m3u8_tests PRIVATE Trackknife::BenchUi Qt6::Test
        trackknife_project_options trackknife_project_warnings)
    target_compile_definitions(trackknife_m3u8_tests PRIVATE
        TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")
    add_test(NAME m3u8 COMMAND trackknife_m3u8_tests)
    # ADR-0220: the library panel routed through an engine by a setting.
    qt_add_executable(trackknife_library_panel_engine_tests library_panel_engine_test.cpp)
    target_link_libraries(
        trackknife_library_panel_engine_tests
        PRIVATE Trackknife::BenchUi Trackknife::Engine Trackknife::Protocol Qt6::Test
                PkgConfig::SQLITE3
                trackknife_project_options trackknife_project_warnings
    )
    target_compile_definitions(trackknife_library_panel_engine_tests PRIVATE
        TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")
    add_test(NAME library-panel-engine COMMAND trackknife_library_panel_engine_tests)
    set_tests_properties(library-panel-engine PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

    # ADR-0220 Phase 2: playing a track drives the engine's player, not the
    # window's own.
    qt_add_executable(trackknife_engine_playback_tests engine_playback_test.cpp)
    target_link_libraries(
        trackknife_engine_playback_tests
        PRIVATE Trackknife::BenchUi Trackknife::Engine Trackknife::Protocol Qt6::Test
                trackknife_project_options trackknife_project_warnings
    )
    target_compile_definitions(trackknife_engine_playback_tests PRIVATE
        TRACKKNIFE_AUDIO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")
    add_test(NAME engine-playback COMMAND trackknife_engine_playback_tests)
    set_tests_properties(engine-playback PROPERTIES TIMEOUT 120 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

    # ADR-0220 Phase 2: the Qt side against a real engine over a real socket.
    qt_add_executable(trackknife_engine_connection_tests engine_connection_test.cpp)
    target_link_libraries(
        trackknife_engine_connection_tests
        PRIVATE Trackknife::UiCommon Trackknife::Engine Trackknife::Protocol Qt6::Test
                trackknife_project_options trackknife_project_warnings
    )
    add_test(NAME engine-connection COMMAND trackknife_engine_connection_tests)
    set_tests_properties(engine-connection PROPERTIES TIMEOUT 60 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

    qt_add_executable(trackknife_local_list_edit_tests local_list_edit_test.cpp)
    target_link_libraries(trackknife_local_list_edit_tests PRIVATE Trackknife::BenchUi Qt6::Test
        trackknife_project_options trackknife_project_warnings)
    add_test(NAME local-list-edit COMMAND trackknife_local_list_edit_tests)

    qt_add_executable(trackknife_track_list_find_tests track_list_find_test.cpp)
    target_link_libraries(trackknife_track_list_find_tests PRIVATE Trackknife::BenchUi Qt6::Test
        trackknife_project_options trackknife_project_warnings)
    add_test(NAME track-list-find COMMAND trackknife_track_list_find_tests)
    set_tests_properties(m3u8 local-list-edit track-list-find PROPERTIES TIMEOUT 30 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
    set_tests_properties(
        bench-main-window
        PROPERTIES
            TIMEOUT 180
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
    )

    add_test(
        NAME ui-smoke
        COMMAND trackknife --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ui_smoke_screenshot.png"
    )
    set_tests_properties(
        ui-smoke
        PROPERTIES
            TIMEOUT 30
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
    )
endif()

add_test(
    NAME spdx-headers
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_spdx.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_executable(trackknife_debug_log_tests debug_log_test.cpp)
target_link_libraries(
    trackknife_debug_log_tests
    PRIVATE Trackknife::UiCommon trackknife_project_options trackknife_project_warnings
)
add_test(NAME debug-log COMMAND trackknife_debug_log_tests)

add_executable(trackknife_discovery_tests discovery_test.cpp)
target_link_libraries(
    trackknife_discovery_tests
    PRIVATE Trackknife::Discovery Trackknife::Core trackknife_project_options
            trackknife_project_warnings
)
add_test(NAME discovery COMMAND trackknife_discovery_tests)
set_tests_properties(discovery PROPERTIES TIMEOUT 60)

# No test writes into the real home's data: every one gets data and cache
# directories of its own under the build tree. Tests once left databases and
# engine logs in ~/.local/share. The config directory stays the real one:
# fontconfig reads it, and the layout tests measure rows in the desktop's
# font; settings are isolated by the tests themselves.
get_property(trackknife_all_tests DIRECTORY PROPERTY TESTS)
foreach(trackknife_test IN LISTS trackknife_all_tests)
    set(trackknife_test_home "${CMAKE_CURRENT_BINARY_DIR}/test-home/${trackknife_test}")
    set_property(TEST ${trackknife_test} APPEND PROPERTY ENVIRONMENT
        "XDG_DATA_HOME=${trackknife_test_home}/data"
        "XDG_CACHE_HOME=${trackknife_test_home}/cache"
        # Engines a test starts are announced under a service of the test's
        # own: not found by the real ones on the network, nor by other tests.
        "TRACKKNIFE_DISCOVERY_SERVICE=_mt-${trackknife_test}._tcp")
endforeach()
