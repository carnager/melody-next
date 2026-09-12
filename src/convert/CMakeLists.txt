# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_convert STATIC
    src/artwork.cpp
    src/convert.cpp
    src/preset.cpp
    src/scan.cpp
)
add_library(Trackknife::Convert ALIAS trackknife_convert)

target_include_directories(
    trackknife_convert
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_convert
    PUBLIC Trackknife::Core Trackknife::Formats Trackknife::Metadata trackknife_project_options
    PRIVATE PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE
            PkgConfig::TAGLIB
            trackknife_project_warnings
)
target_compile_features(trackknife_convert PUBLIC cxx_std_23)
