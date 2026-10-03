# SPDX-License-Identifier: GPL-3.0-only

# Engines announcing themselves, and agents and clients finding them, on
# multicast DNS -- built in, so nothing else need run for it.
find_package(Threads REQUIRED)

add_library(trackknife_discovery STATIC src/dns.cpp src/mdns.cpp)
add_library(Trackknife::Discovery ALIAS trackknife_discovery)

target_include_directories(
    trackknife_discovery
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_discovery
    PUBLIC Trackknife::Core trackknife_project_options
    PRIVATE Threads::Threads trackknife_project_warnings
)
target_compile_features(trackknife_discovery PUBLIC cxx_std_23)

if(NOT TRACKKNIFE_CLI_ONLY AND TRACKKNIFE_ENABLE_UPNP)
    add_library(trackknife_upnp STATIC src/upnp.cpp)
    add_library(Trackknife::UPnP ALIAS trackknife_upnp)
    target_include_directories(trackknife_upnp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(trackknife_upnp PUBLIC Trackknife::Core PRIVATE PkgConfig::UPNP Threads::Threads trackknife_project_warnings)
endif()
