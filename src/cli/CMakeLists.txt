# SPDX-License-Identifier: GPL-3.0-only

# melody-cli: an engine from the shell.
add_executable(melody_cli main.cpp config_command.cpp convert_command.cpp help.cpp)
set_target_properties(melody_cli PROPERTIES OUTPUT_NAME melody-cli)
target_link_libraries(
    melody_cli
    PRIVATE trackknife_daemon_settings Trackknife::Protocol Trackknife::Discovery Trackknife::Core
            trackknife_project_options trackknife_project_warnings
)

install(TARGETS melody_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
# The album page, built on melody-cli.
install(PROGRAMS ${PROJECT_SOURCE_DIR}/scripts/melody-musiclist
        DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
# ADR-0342: completion for bash and zsh, where each looks for it.
install(FILES ${PROJECT_SOURCE_DIR}/packaging/completions/melody-cli.bash
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME melody-cli
        COMPONENT cli)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/completions/_melody-cli
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions COMPONENT cli)
