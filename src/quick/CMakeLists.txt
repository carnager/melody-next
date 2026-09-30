# SPDX-License-Identifier: GPL-3.0-only
#
# The Trackknife window in Qt Quick (ADR-0220): a second view of the one
# workspace, drawn by QML. Everything it does is the workspace's, shared with
# the widgets window; this is only what draws it. Qt Widgets is linked for
# QApplication and the desktop style's icons, never for widgets.

qt_policy(SET QTP0004 NEW)

qt_add_executable(trackknife-quick main.cpp)

# The Trackknife style (ADR-0240): flat controls drawn from the palette,
# Fusion for anything it does not draw itself.
qt_add_library(trackknife_quick_style STATIC)
set_source_files_properties(style/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(
    trackknife_quick_style
    URI Trackknife.Style
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    IMPORTS QtQuick.Controls.Fusion
    SOURCES
    style/pointer.cpp
    style/pointer.hpp
    QML_FILES
    style/Theme.qml
    style/Chevron.qml
    style/CheckMark.qml
    style/FolderGlyph.qml
    style/Button.qml
    style/ToolButton.qml
    style/TextField.qml
    style/TextArea.qml
    style/CheckBox.qml
    style/ComboBox.qml
    style/SpinBox.qml
    style/TabBar.qml
    style/TabButton.qml
    style/ItemDelegate.qml
    style/Menu.qml
    style/MenuItem.qml
    style/MenuSeparator.qml
    style/MenuBar.qml
    style/MenuBarItem.qml
    style/ScrollBar.qml
    style/Dialog.qml
    style/DialogButtonBox.qml
    style/ToolTip.qml
    style/GroupBox.qml
    style/Frame.qml
    style/Pane.qml
    style/Popup.qml
    style/ProgressBar.qml
    style/Slider.qml
    style/ToolBar.qml
    style/ToolSeparator.qml
    style/SplitView.qml
)
target_include_directories(trackknife_quick_style PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/style)
target_link_libraries(trackknife_quick_style PRIVATE Qt6::Quick Qt6::QuickControls2
                                                 )

set_source_files_properties(qml/Shade.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(
    trackknife-quick
    URI Trackknife.Quick
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    SOURCES
    image_providers.cpp
    image_providers.hpp
    keyed_rows_model.cpp
    keyed_rows_model.hpp
    list_tabs_model.cpp
    list_tabs_model.hpp
    quick_artwork.cpp
    quick_artwork.hpp
    quick_dynamic.cpp
    quick_dynamic.hpp
    quick_identify.cpp
    quick_identify.hpp
    quick_convert.cpp
    quick_convert.hpp
    quick_open_list.cpp
    quick_open_list.hpp
    quick_pick.cpp
    quick_pick.hpp
    quick_replaygain.cpp
    quick_search.cpp
    quick_search.hpp
    quick_replaygain.hpp
    quick_script.cpp
    quick_settings.cpp
    quick_settings.hpp
    quick_script.hpp
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
    qml/InitialsTile.qml
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
    qml/IdentifyDialog.qml
    qml/ScriptEditor.qml
    qml/ReplayGainDialog.qml
    qml/ConvertDialog.qml
    qml/KeyedAction.qml
    qml/LibraryFolders.qml
    qml/EngineFolderDialog.qml
    qml/SettingsWindow.qml
    qml/PlaylistTransferBar.qml
    qml/OpenListDialog.qml
    qml/FindBar.qml
    qml/QuickPickPopup.qml
    qml/CommandPalette.qml
    qml/SearchWindow.qml
    qml/DynamicPlaylistsWindow.qml
    qml/ListsPane.qml
    qml/FormLabel.qml
    qml/Hint.qml
    qml/WindowFooter.qml
    qml/DragSource.qml
    qml/DropMarker.qml
)

target_include_directories(trackknife-quick PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
    trackknife-quick
    PRIVATE Trackknife::Workspace Qt6::Widgets Qt6::Quick Qt6::QuickControls2
            trackknife_quick_styleplugin
            trackknife_project_options
)

# Packaged with the window, as "app": the Qt Quick window beside it, a
# preview (ADR-0240), with its own desktop entry.
install(TARGETS trackknife-quick RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/arch/trackknife-quick.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT app)
