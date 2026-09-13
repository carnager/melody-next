# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_metadata STATIC
    src/artwork_composition.cpp
    src/artwork.cpp
    src/artwork_write_plan.cpp
    src/capture_pattern.cpp
    src/document.cpp
    src/draft_document.cpp
    src/field_suggestions.cpp
    src/flac_mapping.cpp
    src/flac_writer.cpp
    src/apev2_trailer_detail.hpp
    src/mp3_writer.cpp
    src/ogg_stream_detail.hpp
    src/container_artwork_writer.cpp
    src/mp4_writer.cpp
    src/ogg_writer.cpp
    src/text_writer_detail.hpp
    src/wavpack_writer.cpp
    src/local_reader.cpp
    src/loudness_sidecar.cpp
    src/proposal.cpp
    src/rule_script_import.cpp
    src/staged_patch.cpp
    src/staged_selection.cpp
    src/transformation.cpp
    src/write_plan.cpp
)
add_library(Trackknife::Metadata ALIAS trackknife_metadata)

target_include_directories(
    trackknife_metadata
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_metadata
    PUBLIC Trackknife::Core Trackknife::TitleFormat trackknife_project_options
    PRIVATE PkgConfig::AVUTIL PkgConfig::TAGLIB trackknife_project_warnings
)
target_compile_features(trackknife_metadata PUBLIC cxx_std_23)
