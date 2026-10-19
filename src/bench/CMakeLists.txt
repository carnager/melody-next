# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_bench_ui STATIC
    about_dialog.cpp
    about_dialog.hpp
    apply_actions_popover.cpp
    apply_actions_popover.hpp
    bench_main_window.cpp
    bench_main_window.hpp
    bench_list_tabs.cpp
    bench_media_ingest.cpp
    bench_remote.cpp
    bench_playlists.cpp
    bench_plugins.cpp
    bench_plugin_triggers.cpp
    plugin_list_window.cpp
    plugin_panel_view.cpp
    plugin_list_window.hpp
    plugin_panel_view.hpp
    playlist_transfer_bar.cpp
    replaygain_dialog.cpp
    themed_icon.cpp
    themed_icon.hpp
    trackknife_style.cpp
    trackknife_style.hpp
    widget_color_scheme.cpp
    widget_color_scheme.hpp
    search_dialog.cpp
    playlist_transfer_bar.hpp
    bench_metadata_operations.cpp
    bench_dynamic_playlists.cpp
    dynamic_playlist_dialog.cpp
    bench_track_views.cpp
    bench_lastfm.cpp
    bench_up_next.cpp
    bench_lyrics.cpp
    lyrics_panel.cpp
    lyrics_panel.hpp
    up_next_delegate.cpp
    quick_pick_popup.cpp
    quick_pick_popup.hpp
    up_next_delegate.hpp
    bench_transport.cpp
    bench_workspace_layout.cpp
    playback_tab_widget.hpp
    file_scope_view.hpp
    convert_dialog.cpp
    convert_dialog.hpp
    lists_panel.cpp
    bench_lists_panel.cpp
    local_list_edit_bar.cpp
    local_list_edit_bar.hpp
    track_list_find_bar.cpp
    track_list_find_bar.hpp
    library_views_dialog.cpp
    list_view_apply.hpp
    list_view_dialog.cpp
    list_view_dialog.hpp
    library_views_dialog.hpp
    local_library_panel.cpp
    local_library_panel.hpp
    cover_thumbnail.cpp
    cover_thumbnail.hpp
    cover_review.cpp
    metadata_artwork_section.cpp
    metadata_artwork_section.hpp
    metadata_exact_value_dialog.cpp
    metadata_exact_value_dialog.hpp
    metadata_field_review_bar.cpp
    metadata_field_review_bar.hpp
    metadata_properties_dialog.cpp
    metadata_properties_dialog.hpp
    metadata_rule_script_import_dialog.cpp
    metadata_rule_script_import_dialog.hpp
    metadata_scalar_delegate.cpp
    metadata_scalar_delegate.hpp
    engine_folder_dialog.cpp
    output_profiles_widget.cpp
    output_profiles_widget.hpp
    single_instance.cpp
    single_instance.hpp
    undo_location_widget.cpp
    undo_location_widget.hpp
    shortcut_settings.cpp
    shortcut_settings.hpp
    settings_dialog.cpp
    settings_dialog.hpp
    metadata_transformation_dialog.cpp
    metadata_transformation_dialog.hpp
    identify_albums_dialog.cpp
    musicbrainz_identify_dialog.cpp
    musicbrainz_identify_dialog.hpp
    musicbrainz_track_match_widget.cpp
    musicbrainz_track_match_widget.hpp
    preparation_feedback_dialog.cpp
    preparation_feedback_dialog.hpp
)
add_library(Trackknife::BenchUi ALIAS trackknife_bench_ui)

target_link_libraries(
    trackknife_bench_ui
    PUBLIC Trackknife::Workspace Trackknife::Lists Qt6::Widgets Trackknife::Core Trackknife::UiCommon Trackknife::Engine Trackknife::Discovery Trackknife::Persistence
           Trackknife::Convert Trackknife::Formats Trackknife::Loudness Trackknife::Metadata Trackknife::MusicBrainz
           Trackknife::Operations Trackknife::Audio
           trackknife_project_options
    PRIVATE Qt6::Concurrent Qt6::DBus Qt6::Network Qt6::Svg
            trackknife_project_warnings
)
target_include_directories(trackknife_bench_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_bench_ui PUBLIC cxx_std_23)

# ADR-0287: the vinyl bird and the mascot, for the window and About Melody.
qt_add_resources(
    trackknife_bench_ui "melody_icons"
    PREFIX "/melody"
    BASE "${PROJECT_SOURCE_DIR}/packaging/icons"
    FILES "${PROJECT_SOURCE_DIR}/packaging/icons/melody.svg"
          "${PROJECT_SOURCE_DIR}/packaging/icons/melody-mascot.svg"
)
# The release and the commit it was built from, for About Melody.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE melody_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
)
target_compile_definitions(trackknife_bench_ui PRIVATE MELODY_RELEASE="${PROJECT_VERSION}"
                                                       MELODY_COMMIT="${melody_commit}")

qt_add_executable(trackknife main.cpp)
target_link_libraries(
    trackknife
    PRIVATE Trackknife::BenchUi Qt6::Widgets trackknife_project_options
            trackknife_project_warnings
)
set_target_properties(trackknife PROPERTIES OUTPUT_NAME melody)

# Packaged as "app": the window, its desktop entry, and the documents.
install(TARGETS trackknife RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/arch/melody.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT app)
# ADR-0288: the official plugins, found under the system's data folder and
# off until enabled.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/plugins/web-search
                  ${PROJECT_SOURCE_DIR}/plugins/about
                  ${PROJECT_SOURCE_DIR}/plugins/lrclib
                  ${PROJECT_SOURCE_DIR}/plugins/scripts
        DESTINATION ${CMAKE_INSTALL_DATADIR}/melody/plugins COMPONENT app
        USE_SOURCE_PERMISSIONS PATTERN "__pycache__" EXCLUDE)
# ADR-0287: the vinyl bird, for the desktop entry and the window.
install(FILES ${PROJECT_SOURCE_DIR}/packaging/icons/melody.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT app)
