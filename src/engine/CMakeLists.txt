# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_library(trackknife_engine STATIC src/catalogue.cpp src/catalogue_methods.cpp src/job_methods.cpp src/server.cpp src/token.cpp src/job_registry.cpp src/playback_methods.cpp src/playback_store.cpp src/player.cpp src/recorder.cpp src/remote_catalogue.cpp src/workspace.cpp src/outputs.cpp src/stream_server.cpp)
add_library(Trackknife::Engine ALIAS trackknife_engine)

target_include_directories(
    trackknife_engine
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_engine
    PUBLIC Trackknife::Audio Trackknife::Output Trackknife::Core Trackknife::Persistence Trackknife::Protocol
           Trackknife::Query
           trackknife_project_options
    PRIVATE Threads::Threads trackknife_project_warnings
)
target_compile_features(trackknife_engine PUBLIC cxx_std_23)
