# SPDX-License-Identifier: GPL-3.0-only

# melody-cli: an engine from the shell.
add_executable(melody_cli main.cpp config_command.cpp)
set_target_properties(melody_cli PROPERTIES OUTPUT_NAME melody-cli)
target_link_libraries(
    melody_cli
    PRIVATE trackknife_daemon_settings Trackknife::Protocol Trackknife::Discovery Trackknife::Core
            trackknife_project_options trackknife_project_warnings
)

# ADR-0286: the skeleton a new melodyd.conf starts from, built in, so
# melody-cli config has it wherever it runs; installed beside it to read.
set(melodyd_skeleton_source ${PROJECT_SOURCE_DIR}/packaging/melodyd.conf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${melodyd_skeleton_source})
file(READ ${melodyd_skeleton_source} MELODYD_SKELETON)
configure_file(melodyd_skeleton.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/melodyd_skeleton.hpp @ONLY)
target_include_directories(melody_cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
install(FILES ${melodyd_skeleton_source} DESTINATION ${CMAKE_INSTALL_DATADIR}/melody
        COMPONENT engine)

install(TARGETS melody_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
# The album page, built on melody-cli.
install(PROGRAMS ${PROJECT_SOURCE_DIR}/scripts/melody-musiclist
        DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
