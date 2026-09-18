# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_query STATIC
    src/tkq_lexer.cpp
    src/tkq_melody.cpp
    src/tkq_parser.cpp
)
add_library(Trackknife::Query ALIAS trackknife_query)

target_include_directories(
    trackknife_query
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_query
    PUBLIC Trackknife::Core Trackknife::TitleFormat trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_query PUBLIC cxx_std_23)
