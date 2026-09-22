# SPDX-License-Identifier: GPL-3.0-only

add_library(trackknife_protocol STATIC src/message.cpp)
add_library(Trackknife::Protocol ALIAS trackknife_protocol)

target_include_directories(
    trackknife_protocol
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_protocol
    PUBLIC Trackknife::Core nlohmann_json::nlohmann_json trackknife_project_options
    PRIVATE trackknife_project_warnings
)
target_compile_features(trackknife_protocol PUBLIC cxx_std_23)
