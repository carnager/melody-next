# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_audio STATIC
    src/local_audition.cpp
    src/local_playback.cpp
    src/melody_agent.cpp
    src/playback_order.cpp
    src/playback_selection.cpp
    src/pipewire_output.cpp
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
    PRIVATE PkgConfig::PIPEWIRE trackknife_project_warnings
)
target_compile_features(trackknife_audio PUBLIC cxx_std_23)
