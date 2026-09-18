# SPDX-License-Identifier: GPL-3.0-only

add_library(
    trackknife_persistence STATIC
    src/file_publication_journal.cpp
    src/list_repository.cpp
    src/local_library.cpp
    src/musicbrainz_cache.cpp
    src/operation_journal.cpp
    src/rating_identity.cpp
    src/tkq_row.cpp
    src/workspace_backup.cpp
)
add_library(Trackknife::Persistence ALIAS trackknife_persistence)

target_include_directories(
    trackknife_persistence
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    trackknife_persistence
    PUBLIC Trackknife::Convert Trackknife::Core Trackknife::Metadata Trackknife::Operations
           Trackknife::Query trackknife_project_options
    PRIVATE PkgConfig::SQLITE3 Trackknife::Formats trackknife_project_warnings
)
target_compile_features(trackknife_persistence PUBLIC cxx_std_23)
