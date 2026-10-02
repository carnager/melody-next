# SPDX-License-Identifier: GPL-3.0-only

add_executable(trackknife_fixture_benchmark fixture_benchmark.cpp synthetic_track_fixture.hpp)
target_link_libraries(
    trackknife_fixture_benchmark
    PRIVATE Trackknife::Core trackknife_project_options trackknife_project_warnings
)

add_test(NAME fixture-benchmark-smoke COMMAND trackknife_fixture_benchmark --quick)

add_executable(trackknife_titleformat_benchmark titleformat_benchmark.cpp)
target_link_libraries(
    trackknife_titleformat_benchmark
    PRIVATE Trackknife::TitleFormat trackknife_project_options trackknife_project_warnings
)
add_test(NAME titleformat-benchmark-smoke COMMAND trackknife_titleformat_benchmark --quick)

if(TRACKKNIFE_BUILD_UI)
    qt_add_executable(trackknife_ui_benchmark trackknife_ui_benchmark.cpp)
    target_link_libraries(
        trackknife_ui_benchmark
        PRIVATE Trackknife::BenchUi Qt6::Test trackknife_project_options
                trackknife_project_warnings
    )
    add_test(NAME trackknife-ui-benchmark-smoke COMMAND trackknife_ui_benchmark --quick)
    set_tests_properties(
        trackknife-ui-benchmark-smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

# ADR-0254: grouping a large library into a view.
add_executable(trackknife_library_views_benchmark library_views_benchmark.cpp)
target_link_libraries(
    trackknife_library_views_benchmark
    PRIVATE Trackknife::Engine Trackknife::Protocol PkgConfig::SQLITE3
            trackknife_project_options trackknife_project_warnings
)
add_test(NAME library-views-benchmark-smoke COMMAND trackknife_library_views_benchmark
         "${PROJECT_SOURCE_DIR}/tests/fixtures/audio" --quick)
