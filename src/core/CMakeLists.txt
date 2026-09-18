# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_core STATIC
    src/atomic_rename.cpp
    src/local_sources.cpp
    src/sha256.cpp
    src/stable_id.cpp
    src/unicode.cpp
)
add_library(Trackknife::Core ALIAS trackknife_core)

target_include_directories(
    trackknife_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_core
    PUBLIC trackknife_project_options
    PRIVATE PkgConfig::UTF8PROC trackknife_project_warnings
)
target_compile_features(trackknife_core PUBLIC cxx_std_23)
