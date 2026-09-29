# SPDX-License-Identifier: GPL-3.0-only
#
# The workspace's behaviour without its widgets: engines and their lists,
# playback, the list model with its undo, persistence, the desktop's view
# and the file tools' services. Both windows are drawn over it -- the widgets
# one and the Qt Quick one -- so there is one of each behaviour, not two.
#
# It does not link Qt Widgets, so nothing here can come to depend on them.
# The files keep their places under bench/ and uicommon/ for now, where
# their include paths already point; they move when those are renamed.

qt_add_library(
    trackknife_workspace STATIC
    workspace.cpp
    workspace.hpp
    workspace_connections.cpp
    workspace_desktop.cpp
    workspace_engines.cpp
    workspace_ingest.cpp
    workspace_lists.cpp
    workspace_playback.cpp
    workspace_texts.cpp
    workspace_transport.cpp
    workspace_up_next.cpp
    workspace_view.hpp
    ../bench/artwork_fitting.cpp
    ../bench/artwork_fitting.hpp
    ../bench/bench_main_window_helpers.cpp
    ../bench/bench_main_window_helpers.hpp
    ../bench/catalogue_source.cpp
    ../bench/catalogue_source.hpp
    ../bench/desktop_notifier.cpp
    ../bench/desktop_notifier.hpp
    ../bench/dynamic_playlist_service.cpp
    ../bench/dynamic_playlist_service.hpp
    ../bench/engine_key.hpp
    ../bench/engine_launcher.cpp
    ../bench/engine_launcher.hpp
    ../bench/engine_list_sync.cpp
    ../bench/engine_list_sync.hpp
    ../bench/engine_playback.cpp
    ../bench/engine_playback.hpp
    ../bench/file_work_tools.cpp
    ../bench/file_work_tools.hpp
    ../bench/lastfm_service.cpp
    ../bench/lastfm_service.hpp
    ../bench/local_list_history.cpp
    ../bench/local_list_model.cpp
    ../bench/local_list_model.hpp
    ../bench/local_playback_service.hpp
    ../bench/metadata_dialog_helpers.cpp
    ../bench/metadata_dialog_helpers.hpp
    ../bench/metadata_transformation_preview_model.cpp
    ../bench/metadata_transformation_preview_model.hpp
    ../bench/mpris_service.cpp
    ../bench/mpris_service.hpp
    ../bench/musicbrainz_lookup.hpp
    ../bench/post_back.hpp
    ../bench/preparation_feedback.hpp
    ../bench/remote_engines.cpp
    ../bench/remote_engines.hpp
    ../bench/remote_mount.cpp
    ../bench/remote_mount.hpp
    ../bench/replaygain_scan.cpp
    ../bench/replaygain_scan.hpp
    ../bench/settings_keys.cpp
    ../bench/settings_keys.hpp
    ../uicommon/debug_log.cpp
    ../uicommon/debug_log.hpp
    ../uicommon/engine_key.hpp
    ../uicommon/list_persistence_service.cpp
    ../uicommon/list_persistence_service.hpp
    ../uicommon/local_artwork.cpp
    ../uicommon/local_artwork.hpp
    ../uicommon/local_files_mime_data.hpp
    ../uicommon/local_folder_tree_model.cpp
    ../uicommon/local_folder_tree_model.hpp
    ../uicommon/metadata_transformation_interchange.cpp
    ../uicommon/metadata_transformation_interchange.hpp
    ../uicommon/panel_layout.cpp
    ../uicommon/panel_layout.hpp
    ../uicommon/rating_color.hpp
    ../uicommon/track_row_roles.hpp
    ../uicommon/track_view_layout.cpp
    ../uicommon/track_view_layout.hpp
)
add_library(Trackknife::Workspace ALIAS trackknife_workspace)

target_link_libraries(
    trackknife_workspace
    PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::DBus Qt6::Network
           Trackknife::Lists Trackknife::Core Trackknife::Engine Trackknife::Discovery
           Trackknife::Persistence Trackknife::Protocol Trackknife::Convert Trackknife::Formats
           Trackknife::Loudness Trackknife::Metadata Trackknife::MusicBrainz Trackknife::Operations
           Trackknife::Audio trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_include_directories(trackknife_workspace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_workspace PUBLIC cxx_std_23)
