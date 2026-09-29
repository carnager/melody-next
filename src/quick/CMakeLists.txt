# SPDX-License-Identifier: GPL-3.0-only
#
# Experiment: the Trackknife window in Qt Quick. A client of the engine's
# protocol and nothing else -- it opens no database and holds no path to one.
# It borrows the widgets window's engine launcher so both find, and if need
# be start, the same engine on this computer.

qt_policy(SET QTP0004 NEW)

qt_add_executable(trackknife-quick main.cpp)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(
    trackknife-quick
    URI Trackknife.Quick
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    SOURCES
    engine_client.cpp
    engine_client.hpp
    engine.cpp
    engine.hpp
    engine_session.cpp
    engine_session.hpp
    player_state.cpp
    player_state.hpp
    lists_model.cpp
    lists_model.hpp
    track_list_model.cpp
    track_list_model.hpp
    library_model.cpp
    library_model.hpp
    up_next_model.cpp
    up_next_model.hpp
    cover_provider.cpp
    cover_provider.hpp
    format.hpp
    QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/Icon.qml
    qml/IconButton.qml
    qml/Cover.qml
    qml/DragGhost.qml
    qml/TkSlider.qml
    qml/PlayerBar.qml
    qml/LibraryPane.qml
    qml/TrackListPane.qml
    qml/UpNextPane.qml
    qml/StatusBar.qml
)

target_include_directories(trackknife-quick PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
    trackknife-quick
    PRIVATE Trackknife::BenchUi Trackknife::Protocol Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes
            trackknife_project_options
)
