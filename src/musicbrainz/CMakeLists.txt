# SPDX-License-Identifier: GPL-3.0-only

# ADR-0237: what an engine without Qt needs -- building an AcoustID request.
add_library(trackknife_musicbrainz_core STATIC src/acoustid_request.cpp
                                              include/trackknife/musicbrainz/acoustid.hpp)
add_library(Trackknife::MusicBrainzCore ALIAS trackknife_musicbrainz_core)
target_include_directories(
    trackknife_musicbrainz_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_musicbrainz_core
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_musicbrainz_core PUBLIC cxx_std_23)

if(NOT TRACKKNIFE_BUILD_UI)
    return()
endif()

qt_add_library(
    trackknife_musicbrainz STATIC
    src/album_groups.cpp
    src/client.cpp
    src/matching.cpp
    src/proposal_bridge.cpp
    src/acoustid.cpp
    src/web_service.cpp
    include/trackknife/musicbrainz/album_groups.hpp
    include/trackknife/musicbrainz/client.hpp
    include/trackknife/musicbrainz/matching.hpp
    include/trackknife/musicbrainz/proposal_bridge.hpp
    include/trackknife/musicbrainz/acoustid.hpp
    include/trackknife/musicbrainz/web_service.hpp
)
add_library(Trackknife::MusicBrainz ALIAS trackknife_musicbrainz)

target_include_directories(
    trackknife_musicbrainz
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_musicbrainz
    PUBLIC Trackknife::Core Trackknife::Metadata Trackknife::MusicBrainzCore Qt6::Core
           trackknife_project_options
    PRIVATE Qt6::Network Trackknife::Loudness trackknife_project_warnings
)
target_compile_features(trackknife_musicbrainz PUBLIC cxx_std_23)
