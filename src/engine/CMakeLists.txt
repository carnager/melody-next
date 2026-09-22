# SPDX-License-Identifier: GPL-3.0-only

find_package(Threads REQUIRED)

add_library(trackknife_engine STATIC src/catalogue.cpp src/catalogue_methods.cpp src/job_methods.cpp src/server.cpp src/job_registry.cpp src/workspace.cpp)
add_library(Trackknife::Engine ALIAS trackknife_engine)

target_include_directories(
    trackknife_engine
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_engine
    PUBLIC Trackknife::Core Trackknife::Persistence Trackknife::Protocol Trackknife::Query
           trackknife_project_options
    PRIVATE Threads::Threads trackknife_project_warnings
)
target_compile_features(trackknife_engine PUBLIC cxx_std_23)
