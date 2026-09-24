# SPDX-License-Identifier: GPL-3.0-only

add_executable(trackknife_titleformat_cli titleformat_cli.cpp)
target_link_libraries(
    trackknife_titleformat_cli
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
set_target_properties(trackknife_titleformat_cli PROPERTIES OUTPUT_NAME trackknife-titleformat)

install(TARGETS trackknife_titleformat_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT app)
