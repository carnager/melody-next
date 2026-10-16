# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_audio STATIC
    src/album_grouping.cpp
    src/local_audition.cpp
    src/local_playback.cpp
    src/offline_output.cpp
    src/output.cpp
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

if(MELODY_ENABLE_LOCAL_AUDIO)
    if(APPLE)
        target_sources(trackknife_audio PRIVATE src/coreaudio_output.cpp)
        find_library(MELODY_AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
        find_library(MELODY_AUDIOUNIT_FRAMEWORK AudioUnit REQUIRED)
        find_library(MELODY_COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
        find_library(MELODY_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_link_libraries(
            trackknife_audio
            PRIVATE
                "${MELODY_AUDIOTOOLBOX_FRAMEWORK}"
                "${MELODY_AUDIOUNIT_FRAMEWORK}"
                "${MELODY_COREAUDIO_FRAMEWORK}"
                "${MELODY_COREFOUNDATION_FRAMEWORK}"
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(trackknife_audio PRIVATE src/pipewire_output.cpp)
        target_link_libraries(trackknife_audio PRIVATE PkgConfig::PIPEWIRE)
    else()
        message(FATAL_ERROR "Local audio is not implemented on ${CMAKE_SYSTEM_NAME}")
    endif()
else()
    target_sources(trackknife_audio PRIVATE src/unavailable_output.cpp)
endif()
