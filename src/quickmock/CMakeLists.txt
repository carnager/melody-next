# SPDX-License-Identifier: GPL-3.0-only
#
# Experiment: a static Qt Quick mockup of the Trackknife main window. It holds
# fake data only and talks to no engine; it exists to judge how the workspace
# looks and feels when drawn by Qt Quick instead of Qt Widgets.

qt_add_executable(trackknife-quick-mockup main.cpp)

set_source_files_properties(Theme.qml MockData.qml App.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(
    trackknife-quick-mockup
    URI Trackknife.Mockup
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
    Main.qml
    Theme.qml
    MockData.qml
    App.qml
    Icon.qml
    IconButton.qml
    Cover.qml
    TkSlider.qml
    PlayerBar.qml
    LibraryPane.qml
    TrackListPane.qml
    UpNextPane.qml
    StatusBar.qml
)

target_link_libraries(trackknife-quick-mockup PRIVATE Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::QuickShapes)
