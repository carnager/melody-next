# SPDX-License-Identifier: GPL-3.0-only
#
# The Trackknife window in Qt Quick (ADR-0220): a second view of the one
# workspace, drawn by QML. Everything it does is the workspace's, shared with
# the widgets window; this is only what draws it. Qt Widgets is linked for
# QApplication and the desktop style's icons, never for widgets.

qt_policy(SET QTP0004 NEW)

qt_add_executable(trackknife-quick main.cpp)

set_source_files_properties(qml/Shade.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(
    trackknife-quick
    URI Trackknife.Quick
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    SOURCES
    image_providers.cpp
    image_providers.hpp
    list_tabs_model.cpp
    list_tabs_model.hpp
    quick_artwork.cpp
    quick_artwork.hpp
    quick_tagger.cpp
    quick_tagger.hpp
    quick_workspace.cpp
    quick_workspace.hpp
    text_rows.cpp
    text_rows.hpp
    track_rows_model.cpp
    track_rows_model.hpp
    QML_FILES
    qml/Main.qml
    qml/Shade.qml
    qml/TransportBar.qml
    qml/LineSlider.qml
    qml/PillButton.qml
    qml/OutputMenu.qml
    qml/TrackTabBar.qml
    qml/TrackTable.qml
    qml/StatusRow.qml
    qml/ReplayGainMenu.qml
    qml/UpNextPanel.qml
    qml/TrackLayoutMenu.qml
    qml/TrackContextMenu.qml
    qml/RatingMenuItem.qml
    qml/SortMenu.qml
    qml/EditBar.qml
    qml/SourcesPanel.qml
    qml/FolderTree.qml
    qml/LibraryPane.qml
    qml/LibraryFoldersDialog.qml
    qml/TaggerWindow.qml
    qml/TaggerActions.qml
    qml/TaggerFieldTable.qml
    qml/TaggerFileList.qml
    qml/FeedbackDialog.qml
    qml/ExactValuesDialog.qml
    qml/AddFieldDialog.qml
    qml/TableDialog.qml
    qml/ArtworkPage.qml
    qml/CompactCover.qml
    qml/CoverPicker.qml
)

target_include_directories(trackknife-quick PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
    trackknife-quick
    PRIVATE Trackknife::Workspace Qt6::Widgets Qt6::Quick Qt6::QuickControls2
            trackknife_project_options
)
