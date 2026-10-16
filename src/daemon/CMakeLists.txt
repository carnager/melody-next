# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

# ADR-0286: melodyd.conf and the command line, apart so it can be tested.
add_library(trackknife_daemon_settings STATIC settings.cpp)
target_include_directories(trackknife_daemon_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    trackknife_daemon_settings
    PUBLIC Trackknife::Core
    PRIVATE trackknife_project_options trackknife_project_warnings
)

add_executable(trackknife_engine_daemon main.cpp)
target_link_libraries(
    trackknife_engine_daemon
    PRIVATE trackknife_daemon_settings Trackknife::Agent Trackknife::Engine Trackknife::Protocol Trackknife::Core Threads::Threads
            trackknife_project_options trackknife_project_warnings
)
# ADR-0220: melodyd is the Go daemon's name until the Phase 6 migration is
# proven. Both must be installable side by side until then.
set_target_properties(trackknife_engine_daemon PROPERTIES OUTPUT_NAME melodyd)

# Packaged as "engine": the daemon and its systemd user unit.
install(TARGETS trackknife_engine_daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT engine)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/systemd/melodyd.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user COMPONENT engine)
# ADR-0260: the release engine.info reports, for logs and bug reports.
target_compile_definitions(trackknife_engine_daemon PRIVATE
    TRACKKNIFE_RELEASE="${PROJECT_VERSION}")
