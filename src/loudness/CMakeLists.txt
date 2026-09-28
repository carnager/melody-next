# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_loudness STATIC
    src/analyzer.cpp
    src/grouping.cpp
    src/replaygain.cpp
    src/scan.cpp
)
add_library(Trackknife::Loudness ALIAS trackknife_loudness)

target_include_directories(
    trackknife_loudness
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_loudness
    PUBLIC Trackknife::Core Trackknife::Formats Trackknife::Metadata trackknife_project_options
    PRIVATE PkgConfig::EBUR128 trackknife_project_warnings
)
target_compile_features(trackknife_loudness PUBLIC cxx_std_23)
