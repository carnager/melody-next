# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(
    trackknife_ui_common STATIC
    application_style.cpp
    application_style.hpp
    command_palette.cpp
    command_palette.hpp
    eliding_label.cpp
    eliding_label.hpp
    flat_header_style.hpp
    line_slider.cpp
    line_slider.hpp
    library_tree_view.cpp
    library_tree_view.hpp
    queue_item_delegate.cpp
    queue_item_delegate.hpp
    delegate_selection_style.hpp
    queue_table_view.cpp
    queue_table_view.hpp
    rating_stars.cpp
    rating_stars.hpp
)
add_library(Trackknife::UiCommon ALIAS trackknife_ui_common)

target_link_libraries(
    trackknife_ui_common
    PUBLIC Qt6::Widgets Trackknife::Workspace Trackknife::Core Trackknife::Engine Trackknife::Persistence
           Trackknife::Protocol trackknife_project_options
    PRIVATE Qt6::Concurrent Trackknife::Formats trackknife_project_warnings
)
target_include_directories(trackknife_ui_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(trackknife_ui_common PUBLIC cxx_std_23)
