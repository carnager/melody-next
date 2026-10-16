# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

# ADR-0286: melodyd.conf and the command line, apart so it can be tested.
add_library(trackknife_daemon_settings STATIC settings.cpp)
target_include_directories(trackknife_daemon_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The skeleton a new melodyd.conf starts from, built in -- melody-cli config
# and melodyd's config.set have it wherever they run -- and installed to read.
set(melodyd_skeleton_source ${PROJECT_SOURCE_DIR}/packaging/melodyd.conf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${melodyd_skeleton_source})
file(READ ${melodyd_skeleton_source} MELODYD_SKELETON)
configure_file(melodyd_skeleton.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/melodyd_skeleton.hpp @ONLY)
target_include_directories(trackknife_daemon_settings PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
install(FILES ${melodyd_skeleton_source} DESTINATION ${CMAKE_INSTALL_DATADIR}/melody
        COMPONENT engine)
target_link_libraries(
    trackknife_daemon_settings
    PUBLIC Trackknife::Core
    PRIVATE trackknife_project_options trackknife_project_warnings
)

add_executable(trackknife_engine_daemon main.cpp config_methods.cpp)
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
              ${PROJECT_SOURCE_DIR}/packaging/systemd/melodyd.socket
        DESTINATION lib/systemd/user COMPONENT engine)
# ADR-0286: the socket enabled for every user, so the engine is there on the
# first connection without anyone starting it. Disabled per user with
# systemctl --user mask melodyd.socket.
install(CODE [[
    set(wants "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/systemd/user/sockets.target.wants")
    file(MAKE_DIRECTORY "${wants}")
    file(CREATE_LINK ../melodyd.socket "${wants}/melodyd.socket" SYMBOLIC)
]] COMPONENT engine)
# ADR-0260: the release engine.info reports, for logs and bug reports.
target_compile_definitions(trackknife_engine_daemon PRIVATE
    TRACKKNIFE_RELEASE="${PROJECT_VERSION}")
