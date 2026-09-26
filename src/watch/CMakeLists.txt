# SPDX-License-Identifier: GPL-3.0-only

# ADR-0232: watching music folders where they are, for an engine elsewhere.
# Core and the protocol client only, so the light build (TRACKKNIFE_CLI_ONLY)
# makes it for a NAS without FFmpeg, PipeWire, SQLite or Qt.
add_library(trackknife_watch STATIC src/watch.cpp)
add_library(Trackknife::Watch ALIAS trackknife_watch)
target_include_directories(
    trackknife_watch
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_watch
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_watch PUBLIC cxx_std_23)

add_executable(melody_watch main.cpp)
set_target_properties(melody_watch PROPERTIES OUTPUT_NAME melody-watch)
target_link_libraries(
    melody_watch
    PRIVATE Trackknife::Watch Trackknife::Protocol Trackknife::Core
            trackknife_project_options trackknife_project_warnings
)

install(TARGETS melody_watch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT watch)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/systemd/melody-watch.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user COMPONENT watch)
