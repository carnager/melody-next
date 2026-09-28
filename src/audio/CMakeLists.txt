# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_audio STATIC
    src/album_grouping.cpp
    src/local_audition.cpp
    src/local_playback.cpp
    src/playback_order.cpp
    src/playback_selection.cpp
)
add_library(Trackknife::Audio ALIAS trackknife_audio)

target_include_directories(
    trackknife_audio
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_audio
    PUBLIC Trackknife::Core Trackknife::Formats trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_audio PUBLIC cxx_std_23)

if(TRACKKNIFE_ENABLE_LOCAL_AUDIO)
    target_sources(trackknife_audio PRIVATE src/pipewire_output.cpp)
    target_link_libraries(trackknife_audio PRIVATE PkgConfig::PIPEWIRE)
else()
    target_sources(trackknife_audio PRIVATE src/unavailable_output.cpp)
endif()
