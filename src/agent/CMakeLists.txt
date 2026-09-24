# SPDX-License-Identifier: GPL-3.0-only

# ADR-0228: melody-agent, an output a melodyd plays on.
add_library(trackknife_agent STATIC agent.cpp guests.cpp speaker_arbiter.cpp)
add_library(Trackknife::Agent ALIAS trackknife_agent)
target_include_directories(trackknife_agent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
    trackknife_agent
    PUBLIC Trackknife::Output Trackknife::Engine Trackknife::Discovery trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_agent PUBLIC cxx_std_23)

add_executable(melody_agent main.cpp)
set_target_properties(melody_agent PROPERTIES OUTPUT_NAME melody-agent)
target_link_libraries(
    melody_agent
    PRIVATE Trackknife::Agent trackknife_project_options trackknife_project_warnings
)

# Packaged as "agent": the output agent and its systemd user unit.
install(TARGETS melody_agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT agent)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/systemd/melody-agent.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user COMPONENT agent)
