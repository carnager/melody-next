# SPDX-License-Identifier: GPL-3.0-only
add_library(trackknife_lists STATIC src/edit_plan.cpp src/list_view.cpp src/m3u8.cpp
    src/playlist_file.cpp)
add_library(Trackknife::Lists ALIAS trackknife_lists)
target_include_directories(trackknife_lists PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(trackknife_lists PUBLIC Trackknife::Metadata Trackknife::Formats
    PRIVATE nlohmann_json::nlohmann_json trackknife_project_warnings trackknife_project_options)
target_compile_features(trackknife_lists PUBLIC cxx_std_23)
