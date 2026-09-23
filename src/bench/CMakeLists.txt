# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_bench_ui STATIC
    bench_main_window.cpp
    bench_main_window.hpp
    bench_main_window_helpers.cpp
    bench_main_window_helpers.hpp
    bench_list_tabs.cpp
    bench_media_ingest.cpp
    bench_remote.cpp
    remote_mount.cpp
    bench_playlists.cpp
    playlist_transfer_bar.cpp
    replaygain_dialog.cpp
    replaygain_scan.cpp
    search_dialog.cpp
    playlist_transfer_bar.hpp
    bench_metadata_operations.cpp
    bench_dynamic_playlists.cpp
    dynamic_playlist_service.cpp
    dynamic_playlist_dialog.cpp
    desktop_notifier.cpp
    mpris_service.cpp
    mpris_service.hpp
    bench_track_views.cpp
    bench_lastfm.cpp
    lastfm_service.cpp
    lastfm_service.hpp
    bench_up_next.cpp
    bench_transport.cpp
    bench_workspace_layout.cpp
    playback_tab_widget.hpp
    file_scope_view.hpp
    convert_dialog.cpp
    convert_dialog.hpp
    engine_playback.cpp
    engine_playback.hpp
    catalogue_source.cpp
    catalogue_source.hpp
    local_list_model.cpp
    local_list_history.cpp
    local_list_model.hpp
    local_list_edit_bar.cpp
    local_list_edit_bar.hpp
    track_list_find_bar.cpp
    track_list_find_bar.hpp
    local_library_panel.cpp
    local_library_panel.hpp
    cover_thumbnail.cpp
    cover_thumbnail.hpp
    cover_review.cpp
    artwork_fitting.cpp
    engine_launcher.cpp
    engine_launcher.hpp
    artwork_fitting.hpp
    metadata_artwork_section.cpp
    metadata_artwork_section.hpp
    metadata_dialog_helpers.cpp
    metadata_dialog_helpers.hpp
    metadata_exact_value_dialog.cpp
    metadata_exact_value_dialog.hpp
    metadata_grid_model.cpp
    metadata_grid_model.hpp
    metadata_field_review_bar.cpp
    metadata_field_review_bar.hpp
    metadata_properties_dialog.cpp
    metadata_properties_dialog.hpp
    metadata_rule_script_import_dialog.cpp
    metadata_rule_script_import_dialog.hpp
    metadata_scalar_delegate.cpp
    metadata_scalar_delegate.hpp
    output_profiles_widget.cpp
    output_profiles_widget.hpp
    shortcut_settings.cpp
    shortcut_settings.hpp
    settings_dialog.cpp
    settings_dialog.hpp
    metadata_transformation_dialog.cpp
    metadata_transformation_dialog.hpp
    metadata_transformation_preview_model.cpp
    musicbrainz_fetch_service.cpp
    musicbrainz_fetch_service.hpp
    musicbrainz_identify_dialog.cpp
    musicbrainz_identify_dialog.hpp
    musicbrainz_track_match_widget.cpp
    musicbrainz_track_match_widget.hpp
    metadata_transformation_preview_model.hpp
    preparation_feedback_dialog.cpp
    preparation_feedback_dialog.hpp
)
add_library(Trackknife::BenchUi ALIAS trackknife_bench_ui)

target_link_libraries(
    trackknife_bench_ui
    PUBLIC Trackknife::Lists Qt6::Widgets Trackknife::Core Trackknife::UiCommon Trackknife::Engine Trackknife::Persistence
           Trackknife::Convert Trackknife::Formats Trackknife::Loudness Trackknife::Metadata Trackknife::MusicBrainz
           Trackknife::Operations Trackknife::Audio
           trackknife_project_options
    PRIVATE Qt6::Concurrent Qt6::DBus Qt6::Network
            trackknife_project_warnings
)
target_include_directories(trackknife_bench_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_bench_ui PUBLIC cxx_std_23)

qt_add_executable(trackknife main.cpp)
target_link_libraries(
    trackknife
    PRIVATE Trackknife::BenchUi Qt6::Widgets trackknife_project_options
            trackknife_project_warnings
)
set_target_properties(trackknife PROPERTIES OUTPUT_NAME trackknife)
