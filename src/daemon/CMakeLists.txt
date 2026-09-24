# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_executable(trackknife_engine_daemon main.cpp)
target_link_libraries(
    trackknife_engine_daemon
    PRIVATE Trackknife::Agent Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
# ADR-0220: melodyd is the Go daemon's name until the Phase 6 migration is
# proven. Both must be installable side by side until then.
set_target_properties(trackknife_engine_daemon PROPERTIES OUTPUT_NAME melodyd)
