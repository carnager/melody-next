# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_formats STATIC
    src/artwork.cpp
    src/cover_fitting.cpp
    src/cue_sheet.cpp
    src/decoder.cpp
    src/probe.cpp
)
add_library(Trackknife::Formats ALIAS trackknife_formats)

target_include_directories(
    trackknife_formats
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_formats
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE PkgConfig::SWSCALE
            PkgConfig::OPENMPT trackknife_project_warnings
)
target_compile_features(trackknife_formats PUBLIC cxx_std_23)
