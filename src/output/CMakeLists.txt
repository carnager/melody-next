# SPDX-License-Identifier: GPL-3.0-only

# ADR-0228: what an engine and an output agent share -- the audition wire
# format, the engine's view of an agent, and the agent's request handlers.
# Only the audio side and the protocol: an agent links this, not the engine's
# database and catalogue.
add_library(
    trackknife_output STATIC
    src/agent_audition.cpp
    src/audition_methods.cpp
    src/audition_wire.cpp
    src/stream_query.cpp
)
add_library(Trackknife::Output ALIAS trackknife_output)

target_include_directories(
    trackknife_output
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_output
    PUBLIC Trackknife::Audio Trackknife::Protocol Trackknife::Core trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_output PUBLIC cxx_std_23)
