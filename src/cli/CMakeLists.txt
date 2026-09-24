# SPDX-License-Identifier: GPL-3.0-only

# melody-cli: an engine from the shell.
add_executable(melody_cli main.cpp)
set_target_properties(melody_cli PROPERTIES OUTPUT_NAME melody-cli)
target_link_libraries(
    melody_cli
    PRIVATE Trackknife::Protocol Trackknife::Discovery Trackknife::Core
            trackknife_project_options trackknife_project_warnings
)
