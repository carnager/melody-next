# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_ui_common STATIC
    command_palette.cpp
    command_palette.hpp
    line_slider.cpp
    line_slider.hpp
    library_tree_view.cpp
    library_tree_view.hpp
    list_persistence_service.cpp
    list_persistence_service.hpp
    local_folder_tree_model.cpp
    local_folder_tree_model.hpp
    local_files_mime_data.hpp
    local_artwork.cpp
    local_artwork.hpp
    metadata_transformation_interchange.cpp
    metadata_transformation_interchange.hpp
    panel_layout.cpp
    panel_layout.hpp
    queue_item_delegate.cpp
    queue_item_delegate.hpp
    debug_log.cpp
    debug_log.hpp
    engine_connection.cpp
    engine_connection.hpp
    queue_table_view.cpp
    queue_table_view.hpp
    rating_stars.cpp
    rating_stars.hpp
    track_row_roles.hpp
    track_view_layout.cpp
    track_view_layout.hpp
)
add_library(Trackknife::UiCommon ALIAS trackknife_ui_common)

target_link_libraries(
    trackknife_ui_common
    PUBLIC Qt6::Widgets Trackknife::Core Trackknife::Engine Trackknife::Persistence
           Trackknife::Protocol trackknife_project_options
    PRIVATE Qt6::Concurrent Trackknife::Formats trackknife_project_warnings
)
target_include_directories(trackknife_ui_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_ui_common PUBLIC cxx_std_23)
