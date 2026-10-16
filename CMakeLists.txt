# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.28)

project(
    Trackknife
    VERSION 0.3.0
    DESCRIPTION "A fast native Linux and macOS audio player and library multitool"
    LANGUAGES CXX
)

# The macOS desktop integration is Objective-C++ (ADR-0275).
if(APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(CompilerWarnings)
include(TestIsolation)
include(Sanitizers)

option(MELODY_BUILD_UI "Build the Qt Widgets application and UI tests" ON)
option(MELODY_BUILD_BENCHMARKS "Build generated-fixture benchmarks" ON)
option(MELODY_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)
option(MELODY_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(MELODY_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
# ADR-0235: optional. AUTO builds it where libupnp is installed and leaves it
# out where it is not; ON insists on it, OFF never builds it.
set(MELODY_ENABLE_UPNP AUTO CACHE STRING "Build UPnP renderer discovery and playback")
set_property(CACHE MELODY_ENABLE_UPNP PROPERTY STRINGS AUTO ON OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    set(MELODY_LOCAL_AUDIO_DEFAULT ON)
    set(MELODY_WATCH_DEFAULT ON)
else()
    set(MELODY_LOCAL_AUDIO_DEFAULT OFF)
    set(MELODY_WATCH_DEFAULT OFF)
endif()
option(MELODY_ENABLE_LOCAL_AUDIO "Build the platform local-output backend"
       ${MELODY_LOCAL_AUDIO_DEFAULT})
option(MELODY_BUILD_WATCH "Build the Linux inotify melody-watch utility"
       ${MELODY_WATCH_DEFAULT})
# melody-cli alone: it needs the protocol and discovery, not the media
# libraries, so a package of just the CLI builds in seconds.
option(MELODY_CLI_ONLY "Build only melody-cli, without the media libraries" OFF)

# Linking is most of an incremental Debug build: about a hundred test binaries
# each link the libraries whole, and lld does each about nine times faster
# than GNU ld. Debug only -- release packages are built with GCC's -flto,
# whose objects lld cannot link -- and only where lld is installed.
set(MELODY_LINKER AUTO CACHE STRING "Linker: AUTO uses lld for Debug builds where found")
set_property(CACHE MELODY_LINKER PROPERTY STRINGS AUTO LLD DEFAULT)
if(MELODY_LINKER STREQUAL "LLD")
    set(CMAKE_LINKER_TYPE LLD)
elseif(MELODY_LINKER STREQUAL "AUTO" AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT APPLE
       AND NOT DEFINED CMAKE_LINKER_TYPE)
    find_program(MELODY_LLD NAMES ld.lld)
    if(MELODY_LLD)
        set(CMAKE_LINKER_TYPE LLD)
    endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET "libutf8proc>=2.9")
if(NOT MELODY_CLI_ONLY)
    pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET "sqlite3>=3.37")
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET "libavformat>=60")
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET "libavcodec>=60")
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET "libavutil>=58")
    pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET "libswresample>=4")
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET "libswscale>=7")
    pkg_check_modules(OPENMPT REQUIRED IMPORTED_TARGET "libopenmpt>=0.7")
    pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET "taglib>=2.0")
    if(MELODY_ENABLE_LOCAL_AUDIO AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET "libpipewire-0.3>=0.3.50")
    endif()
    if(MELODY_ENABLE_UPNP STREQUAL "AUTO")
        pkg_check_modules(UPNP IMPORTED_TARGET "libupnp>=1.14")
        if(UPNP_FOUND)
            set(MELODY_ENABLE_UPNP ON)
        else()
            message(STATUS "libupnp not found: building without UPnP renderers")
            set(MELODY_ENABLE_UPNP OFF)
        endif()
    elseif(MELODY_ENABLE_UPNP)
        pkg_check_modules(UPNP REQUIRED IMPORTED_TARGET "libupnp>=1.14")
    endif()
    pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET "libebur128>=1.2")
    # The engine's Last.fm client: HTTPS, and MD5 for its request signatures.
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET "libcurl>=7.68")
    pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET "libcrypto>=1.1")
endif()
# Settled for every directory below: no renderers in a build without media.
if(MELODY_ENABLE_UPNP STREQUAL "AUTO")
    set(MELODY_ENABLE_UPNP OFF)
endif()
# ADR-0222: protocol v1 is line-delimited JSON. MIT licensed, so GPLv3
# compatible; header-only, and its notice ships with the package.
find_package(nlohmann_json 3.11 REQUIRED)

add_library(trackknife_project_options INTERFACE)
trackknife_enable_sanitizers(trackknife_project_options)
target_compile_definitions(
    trackknife_project_options
    INTERFACE
        MELODY_ENABLE_UPNP=$<BOOL:${MELODY_ENABLE_UPNP}>
        MELODY_ENABLE_LOCAL_AUDIO=$<BOOL:${MELODY_ENABLE_LOCAL_AUDIO}>
)

add_library(trackknife_project_warnings INTERFACE)
trackknife_set_project_warnings(
    trackknife_project_warnings
    ${MELODY_WARNINGS_AS_ERRORS}
)

if(MELODY_ENABLE_CLANG_TIDY)
    find_program(MELODY_CLANG_TIDY clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${MELODY_CLANG_TIDY}")
endif()

add_subdirectory(src/core)
if(MELODY_CLI_ONLY)
    add_subdirectory(src/protocol)
    add_subdirectory(src/discovery)
    # ADR-0286: melodyd.conf's settings, which melody-cli config checks by.
    add_subdirectory(src/daemon)
    add_subdirectory(src/cli)
    if(MELODY_BUILD_WATCH)
        add_subdirectory(src/watch)
    endif()
    return()
endif()
add_subdirectory(src/titleformat)
add_subdirectory(src/query)
add_subdirectory(src/metadata)
add_subdirectory(src/operations)
add_subdirectory(src/persistence)
# ADR-0220: the core's front door. Depends on persistence and query; nothing
# in the UI may reach past it to open a database itself.
add_subdirectory(src/protocol)
add_subdirectory(src/discovery)
add_subdirectory(src/engine)
add_subdirectory(src/formats)
add_subdirectory(src/lists)
add_subdirectory(src/audio)
add_subdirectory(src/output)
add_subdirectory(src/loudness)
add_subdirectory(src/convert)
add_subdirectory(src/tools)
add_subdirectory(src/daemon)
add_subdirectory(src/agent)
add_subdirectory(src/cli)
if(MELODY_BUILD_WATCH)
    add_subdirectory(src/watch)
endif()

if(MELODY_BUILD_UI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Concurrent DBus Gui Network Svg Test Widgets)
    qt_standard_project_setup()
endif()
# Always: the engine builds AcoustID requests with its Qt-free core
# (ADR-0237); its Qt client comes with the UI.
add_subdirectory(src/musicbrainz)
if(MELODY_BUILD_UI)
    add_subdirectory(src/workspace)
    add_subdirectory(src/uicommon)
    add_subdirectory(src/bench)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(MELODY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(MELODY_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

find_program(MELODY_CLANG_FORMAT clang-format)
if(MELODY_CLANG_FORMAT)
    file(
        GLOB_RECURSE MELODY_FORMAT_FILES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/*.hpp"
    )
    add_custom_target(
        format
        COMMAND "${MELODY_CLANG_FORMAT}" -i ${MELODY_FORMAT_FILES}
        COMMENT "Formatting Trackknife C++ sources"
        VERBATIM
    )
    add_custom_target(
        format-check
        COMMAND "${MELODY_CLANG_FORMAT}" --dry-run --Werror ${MELODY_FORMAT_FILES}
        COMMENT "Checking Trackknife C++ formatting"
        VERBATIM
    )
endif()
