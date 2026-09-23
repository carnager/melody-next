// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/engine_launcher.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/metadata_artwork_section.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "trackknife/audio/local_audition.hpp"
#include <QRandomGenerator>

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/command_palette.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/local_folder_tree_model.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QToolButton>

#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <filesystem>
#include <memory>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {} // namespace

namespace {

constexpr auto panel_layout_settings_key = "workspace/panel-layout-v1";
constexpr auto folders_panel_id = "folders";
constexpr auto track_lists_panel_id = "track-lists";
constexpr auto layout_panel_id_property = "trackknife-layout-panel-id";
constexpr auto layout_panel_title_property = "trackknife-layout-panel-title";
constexpr auto layout_container_kind_property = "trackknife-layout-container-kind";

} // namespace

void BenchMainWindow::buildWorkspace() {
    tabs_ = new PlaybackTabWidget(this);
    tabs_->setObjectName(QStringLiteral("bench-tabs"));
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    tabs_->setAcceptDrops(true);
    tabs_->installEventFilter(this);
    tabs_->tabBar()->setAcceptDrops(true);
    tabs_->tabBar()->installEventFilter(this);
    tabs_->tabBar()->setToolTip(
        tr("Drop local tracks on a tab to transfer, or on empty tab-bar space to create a tab. "
           "Hold Ctrl to copy."));
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &BenchMainWindow::closeTabAt);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](const int) {
        rememberTabVisit(tabs_->currentWidget());
        refreshTabActions();
        refreshTrackViewActions();
        refreshSelectionStatus();
        refreshActiveContext();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this,
            [this](const int, const int) { schedulePersist(); });
    tabs_->setProperty(layout_panel_id_property, QString::fromLatin1(track_lists_panel_id));
    tabs_->setProperty(layout_panel_title_property, QStringLiteral("Track Lists"));
    tabs_->setProperty("trackknifeLayoutPanel", true);

    folder_model_ = new ui::LocalFolderTreeModel(this);
    folders_panel_ = new QWidget(this);
    folders_panel_->setObjectName(QStringLiteral("bench-panel-folders"));
    folders_panel_->setProperty(layout_panel_id_property, QString::fromLatin1(folders_panel_id));
    folders_panel_->setProperty(layout_panel_title_property, QStringLiteral("Sources"));
    folders_panel_->setProperty("trackknifeLayoutPanel", true);
    folders_panel_->setMinimumWidth(160);
    auto* folders_layout = new QVBoxLayout(folders_panel_);
    folders_layout->setContentsMargins(0, 0, 0, 0);
    folders_layout->setSpacing(0);
    auto* heading_row = new QHBoxLayout;
    heading_row->setContentsMargins(6, 6, 6, 2);
    heading_row->setSpacing(2);
    // One-click source switching (ADR-0130): a flat tab bar per authority
    // replaces the dropdown and static heading.
    const auto make_source_tabs = [this](const QString& object_name,
                                         const QString& accessible_name) {
        auto* bar = new QTabBar(folders_panel_);
        bar->setObjectName(object_name);
        bar->setAccessibleName(accessible_name);
        bar->setExpanding(true);
        bar->setDrawBase(false);
        bar->setDocumentMode(true);
        // A segmented control: one pill holding the sources, the chosen one
        // filled -- choosing between views, not holding documents.
        const auto ground = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::Text);
        const auto shade = [&](const int amount) {
            const auto mix = [amount](const int a, const int b) {
                return (a * (100 - amount) + b * amount) / 100;
            };
            return QColor::fromRgb(mix(ground.red(), ink.red()), mix(ground.green(), ink.green()),
                                   mix(ground.blue(), ink.blue()))
                .name();
        };
        bar->setAttribute(Qt::WA_StyledBackground);
        bar->setStyleSheet(
            QStringLiteral("QTabBar { background: %1; border-radius: 5px; }"
                           "QTabBar::tab { background: transparent; border: none; margin: 2px;"
                           " padding: 3px 12px; border-radius: 4px; color: palette(placeholder-text); }"
                           "QTabBar::tab:hover { color: palette(text); }"
                           "QTabBar::tab:selected { background: palette(highlight);"
                           " color: palette(highlighted-text); }")
                .arg(shade(6)));
        return bar;
    };
    local_source_tabs_ = make_source_tabs(QStringLiteral("bench-local-source-tabs"),
                                          QStringLiteral("Local music source"));
    local_source_tabs_->addTab(QStringLiteral("Folders"));
    local_source_tabs_->addTab(QStringLiteral("Library"));
    heading_row->addWidget(local_source_tabs_, 1);
    // Remembered only when chosen: tabs appearing, hiding or being switched
    // by the window itself do not change what the user asked for.
    connect(local_source_tabs_, &QTabBar::tabBarClicked, this, [this](const int index) {
        const auto kind = local_source_tabs_->tabData(index).toString();
        // The temporary Files page (ADR-0183 addendum) is session-only.
        const auto choice = kind == QStringLiteral("remote") ? QStringLiteral("remote")
                            : index == 0                    ? QStringLiteral("folders")
                            : index == 1                    ? QStringLiteral("library")
                                                            : QString{};
        if (!choice.isEmpty()) {
            QSettings{}.setValue(QStringLiteral("local-library/view"), choice);
        }
    });
    // Before the window reacts to changes: the rest of it is not built yet.
    selectPreferredSource();
    connect(local_source_tabs_, &QTabBar::currentChanged, this,
            [this](int) { refreshActiveContext(); });
    folders_layout->addLayout(heading_row);
    folder_bookmarks_heading_ = new QLabel(QStringLiteral("Bookmarks"), folders_panel_);
    auto* bookmarks_heading = folder_bookmarks_heading_;
    bookmarks_heading->setObjectName(QStringLiteral("bench-folder-bookmarks-heading"));
    bookmarks_heading->setContentsMargins(8, 4, 8, 2);
    auto bookmarks_font = bookmarks_heading->font();
    bookmarks_font.setPointSizeF(bookmarks_font.pointSizeF() * 0.85);
    bookmarks_font.setBold(true);
    bookmarks_heading->setFont(bookmarks_font);
    folders_layout->addWidget(bookmarks_heading);
    folder_bookmarks_ = new QListWidget(folders_panel_);
    folder_bookmarks_->setObjectName(QStringLiteral("bench-folder-bookmarks"));
    folder_bookmarks_->setAccessibleName(QStringLiteral("Folder bookmarks"));
    folder_bookmarks_->setFrameShape(QFrame::NoFrame);
    folder_bookmarks_->setUniformItemSizes(true);
    folder_bookmarks_->setMaximumHeight(150);
    folder_bookmarks_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    folder_bookmarks_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    folder_bookmarks_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folder_bookmarks_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showFolderBookmarkMenu);
    connect(folder_bookmarks_, &QListWidget::activated, this, [this](const QModelIndex& index) {
        const auto* item = folder_bookmarks_->item(index.row());
        if (item == nullptr) {
            return;
        }
        const auto bytes = item->data(Qt::UserRole).toByteArray();
        if (!bytes.isEmpty()) {
            revealFolderPath(
                std::string{bytes.constData(), static_cast<std::size_t>(bytes.size())});
        }
    });
    folders_layout->addWidget(folder_bookmarks_);
    source_stack_ = new QStackedWidget(folders_panel_);
    source_stack_->setObjectName(QStringLiteral("bench-source-stack"));
    folder_view_ = new QTreeView(source_stack_);
    folder_view_->setObjectName(QStringLiteral("bench-folder-tree"));
    folder_view_->setModel(folder_model_);
    folder_view_->setHeaderHidden(true);
    folder_view_->setUniformRowHeights(true);
    folder_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folder_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showFolderContextMenu);
    connect(folder_view_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || folder_model_->isDirectory(index)) {
            return;
        }
        openLocalPaths({folder_model_->rawPath(index)});
    });
    source_stack_->addWidget(folder_view_);
    folders_layout->addWidget(source_stack_, 1);

    panel_widgets_.insert(QString::fromLatin1(folders_panel_id), folders_panel_);
    panel_widgets_.insert(QString::fromLatin1(track_lists_panel_id), tabs_);
    layout_host_ = new QWidget(this);
    layout_host_->setObjectName(QStringLiteral("bench-panel-layout-host"));
    layout_host_layout_ = new QVBoxLayout(layout_host_);
    layout_host_layout_->setContentsMargins(0, 0, 0, 0);
    layout_host_layout_->setSpacing(0);
    setCentralWidget(layout_host_);
    loadPanelLayout();

    selection_status_ = new QLabel(statusBar());
    selection_status_->setObjectName(QStringLiteral("bench-selection-status"));
    selection_status_->setAccessibleName(QStringLiteral("Selected track information"));
    selection_status_->setContentsMargins(6, 0, 6, 0);
    selection_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusBar()->addWidget(selection_status_, 1);
    refreshSelectionStatus();

    folder_bookmark_add_action_ = new QAction(QStringLiteral("Bookmark folder"), this);
    folder_bookmark_add_action_->setObjectName(QStringLiteral("action-folder-bookmark-add"));
    connect(folder_bookmark_add_action_, &QAction::triggered, this, [this] {
        const auto index = folder_view_->currentIndex();
        if (index.isValid() && folder_model_->isDirectory(index)) {
            addFolderBookmark(folder_model_->rawPath(index));
        }
    });
    folder_bookmark_remove_action_ = new QAction(QStringLiteral("Remove bookmark"), this);
    folder_bookmark_remove_action_->setObjectName(QStringLiteral("action-folder-bookmark-remove"));
    connect(folder_bookmark_remove_action_, &QAction::triggered, this, [this] {
        delete folder_bookmarks_->takeItem(folder_bookmarks_->currentRow());
        persistFolderBookmarks();
        folder_bookmarks_->setVisible(folder_bookmarks_->count() > 0);
        folder_bookmarks_heading_->setVisible(folder_bookmarks_->isVisibleTo(folders_panel_));
    });
    folder_bookmark_menu_ = new QMenu(this);
    folder_bookmark_menu_->setObjectName(QStringLiteral("bench-folder-bookmark-menu"));
    folder_bookmark_menu_->addAction(folder_bookmark_remove_action_);
    loadFolderBookmarks();

    // The tree browses the whole filesystem; bookmarks are the fast lane.
    folder_model_->addRoot("/");
    const auto home = QFile::encodeName(QDir::homePath());
    revealFolderPath(std::string{home.constData(), static_cast<std::size_t>(home.size())});

    auto* file_menu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* new_list = file_menu->addAction(QStringLiteral("New list…"));
    new_list->setObjectName(QStringLiteral("action-new-list"));
    new_list->setShortcut(QKeySequence::New);
    connect(new_list, &QAction::triggered, this, &BenchMainWindow::createList);
    auto* open_files = file_menu->addAction(QStringLiteral("Open files…"));
    open_files->setObjectName(QStringLiteral("action-open-files"));
    open_files->setShortcut(QKeySequence::Open);
    connect(open_files, &QAction::triggered, this, &BenchMainWindow::openFilesDialog);
    auto* open_folder = file_menu->addAction(QStringLiteral("Open folder…"));
    open_folder->setObjectName(QStringLiteral("action-open-folder"));
    open_folder->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(open_folder, &QAction::triggered, this, &BenchMainWindow::openFolderDialog);
    buildPlaylistActions(file_menu);
    auto* dynamic = file_menu->addAction(QStringLiteral("Dynamic playlists…"));
    dynamic->setObjectName(QStringLiteral("action-dynamic-playlists"));
    connect(dynamic, &QAction::triggered, this, &BenchMainWindow::showDynamicPlaylists);
    auto* backup_workspace = file_menu->addAction(QStringLiteral("Back up workspace database…"));
    backup_workspace->setObjectName(QStringLiteral("action-backup-workspace"));
    connect(backup_workspace, &QAction::triggered, this, &BenchMainWindow::backupWorkspace);
    auto* restore_workspace = file_menu->addAction(QStringLiteral("Restore workspace database…"));
    restore_workspace->setObjectName(QStringLiteral("action-restore-workspace"));
    connect(restore_workspace, &QAction::triggered, this,
            &BenchMainWindow::scheduleWorkspaceRestore);
    auto* add_root = file_menu->addAction(QStringLiteral("Bookmark folder…"));
    connect(add_root, &QAction::triggered, this, &BenchMainWindow::addFolderRoot);
    file_menu->addSeparator();
    file_menu->addSeparator();
    auto* quit = file_menu->addAction(QStringLiteral("Quit"));
    quit->setObjectName(QStringLiteral("action-quit"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setToolTip(QStringLiteral("Close Trackknife and stop this computer's engine; closing "
                                    "the window leaves the music playing"));
    connect(quit, &QAction::triggered, this, &BenchMainWindow::quitAndStopEngine);

    auto* edit_menu = menuBar()->addMenu(QStringLiteral("&Edit"));
    list_find_bar_ = new TrackListFindBar(this);
    addToolBar(Qt::BottomToolBarArea, list_find_bar_);
    list_find_bar_->hide();
    find_list_action_ = edit_menu->addAction(tr("Find in current list…"));
    find_list_action_->setObjectName(QStringLiteral("action-find-in-list"));
    find_list_action_->setShortcut(QKeySequence::Find);
    find_list_action_->setEnabled(false);
    connect(find_list_action_, &QAction::triggered, list_find_bar_, &TrackListFindBar::open);
    find_next_action_ = edit_menu->addAction(tr("Find next in list"));
    find_next_action_->setObjectName(QStringLiteral("action-find-next-in-list"));
    find_next_action_->setShortcut(QKeySequence(Qt::Key_F3));
    find_next_action_->setEnabled(false);
    connect(find_next_action_, &QAction::triggered, this, [this] { list_find_bar_->findNext(); });
    find_previous_action_ = edit_menu->addAction(tr("Find previous in list"));
    find_previous_action_->setObjectName(QStringLiteral("action-find-previous-in-list"));
    find_previous_action_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    find_previous_action_->setEnabled(false);
    connect(find_previous_action_, &QAction::triggered, this,
            [this] { list_find_bar_->findNext(true); });
    edit_menu->addSeparator();
    undo_list_action_ = edit_menu->addAction(QStringLiteral("Undo list edit"));
    undo_list_action_->setObjectName(QStringLiteral("action-undo-list-edit"));
    undo_list_action_->setShortcut(QKeySequence::Undo);
    undo_list_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    undo_list_action_->setEnabled(false);
    connect(undo_list_action_, &QAction::triggered, this, [this] { replayListEdit(true); });
    redo_list_action_ = edit_menu->addAction(QStringLiteral("Redo list edit"));
    redo_list_action_->setObjectName(QStringLiteral("action-redo-list-edit"));
    redo_list_action_->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Shift+Z")), QKeySequence(QStringLiteral("Ctrl+Y"))});
    redo_list_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    redo_list_action_->setEnabled(false);
    connect(redo_list_action_, &QAction::triggered, this, [this] { replayListEdit(false); });
    edit_menu->addSeparator();
    list_edit_bar_ = new LocalListEditBar(this);
    addToolBar(Qt::BottomToolBarArea, list_edit_bar_);
    list_edit_bar_->hide();
    connect(list_edit_bar_, &LocalListEditBar::edited, this, [this](LocalListModel* model) {
        for (const auto& tab : list_tabs_)
            if (tab->model == model) {
                markTabDirty(*tab);
                syncArtwork(*tab);
                break;
            }
        refreshSelectionStatus();
    });
    sort_list_menu_ = edit_menu->addMenu(tr("Sort list"));
    sort_list_menu_->setObjectName(QStringLiteral("bench-sort-list-menu"));
    const auto preset = [this](const QString& name, const char* object, const char* expression) {
        auto* action = sort_list_menu_->addAction(name);
        action->setObjectName(QString::fromLatin1(object));
        connect(action, &QAction::triggered, this, [this, expression] {
            list_edit_bar_->start({.kind = lists::EditKind::sort, .expression = expression});
        });
    };
    preset(tr("Title"), "action-sort-list-title", "%title%");
    preset(tr("Artist / album / track"), "action-sort-list-artist",
           "$if2(%albumartist%,%artist%)|%album%|%discnumber%|%tracknumber%|%title%");
    preset(tr("Album / track"), "action-sort-list-album",
           "%album%|%discnumber%|%tracknumber%|%title%");
    preset(tr("Track number"), "action-sort-list-track", "%discnumber%|%tracknumber%");
    preset(tr("Path"), "action-sort-list-path", "$info(path)");
    sort_list_menu_->addSeparator();
    auto* custom = sort_list_menu_->addAction(tr("Custom expression…"));
    custom->setObjectName(QStringLiteral("action-sort-list-custom"));
    connect(custom, &QAction::triggered, list_edit_bar_, &LocalListEditBar::openSort);
    reverse_list_action_ = edit_menu->addAction(tr("Reverse list"));
    reverse_list_action_->setObjectName(QStringLiteral("action-reverse-list"));
    shuffle_albums_action_ = edit_menu->addAction(tr("Shuffle albums"));
    shuffle_albums_action_->setObjectName(QStringLiteral("action-shuffle-albums"));
    shuffle_albums_action_->setToolTip(tr("Reorder the whole list by album; retain each album's "
                                          "existing track order. Random playback is unchanged."));
    connect(shuffle_albums_action_, &QAction::triggered, this, [this] {
        list_edit_bar_->start({.kind = lists::EditKind::shuffle_albums,
                               .expression = {},
                               .seed = QRandomGenerator::global()->generate()});
    });
    connect(reverse_list_action_, &QAction::triggered, this, [this] {
        list_edit_bar_->start({.kind = lists::EditKind::reverse, .expression = {}});
    });
    deduplicate_list_action_ = edit_menu->addAction(tr("Remove duplicate entries"));
    deduplicate_list_action_->setObjectName(QStringLiteral("action-deduplicate-list"));
    deduplicate_list_action_->setToolTip(
        tr("Keep the first occurrence of each exact local source and logical track"));
    connect(deduplicate_list_action_, &QAction::triggered, this, [this] {
        list_edit_bar_->start({.kind = lists::EditKind::remove_duplicates, .expression = {}});
    });
    edit_menu->addSeparator();
    play_selected_action_ = new QAction(QStringLiteral("Play"), this);
    play_selected_action_->setObjectName(QStringLiteral("action-play-selected-track"));
    connect(play_selected_action_, &QAction::triggered, this, &BenchMainWindow::playCurrentRow);
    properties_action_ = edit_menu->addAction(QStringLiteral("Edit tags…"));
    replaygain_action_ = edit_menu->addAction(QStringLiteral("ReplayGain…"));
    replaygain_action_->setObjectName(QStringLiteral("action-replaygain-dialog"));
    connect(replaygain_action_, &QAction::triggered, this, &BenchMainWindow::showReplayGainDialog);
    properties_action_->setObjectName(QStringLiteral("action-track-properties"));
    properties_action_->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    properties_action_->setEnabled(false);
    connect(properties_action_, &QAction::triggered, this,
            &BenchMainWindow::showMetadataProperties);
    convert_action_ = edit_menu->addAction(QStringLiteral("Convert files…"));
    convert_action_->setObjectName(QStringLiteral("action-convert-files"));
    convert_action_->setEnabled(false);
    connect(convert_action_, &QAction::triggered, this, &BenchMainWindow::showConvertDialog);
    edit_menu->addSeparator();
    auto* settings_action = edit_menu->addAction(QStringLiteral("Settings…"));
    settings_action->setObjectName(QStringLiteral("action-settings"));
    settings_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settings_action, &QAction::triggered, this, [this] { showSettingsDialog(); });
    edit_menu->addSeparator();
    remove_selected_action_ = edit_menu->addAction(QStringLiteral("Remove selected"));
    remove_selected_action_->setObjectName(QStringLiteral("action-remove-selected-tracks"));
    remove_selected_action_->setShortcut(QKeySequence::Delete);
    connect(remove_selected_action_, &QAction::triggered, this,
            &BenchMainWindow::removeSelectedRows);

    folder_add_to_list_action_ = new QAction(QStringLiteral("Add to current list"), this);
    folder_add_to_list_action_->setObjectName(QStringLiteral("action-folder-add-to-list"));
    connect(folder_add_to_list_action_, &QAction::triggered, this, [this] {
        const auto index = folder_view_->currentIndex();
        if (index.isValid()) {
            openLocalPaths({folder_model_->rawPath(index)});
        }
    });
    folder_toggle_expanded_action_ = new QAction(QStringLiteral("Expand"), this);
    folder_toggle_expanded_action_->setObjectName(QStringLiteral("action-folder-toggle-expanded"));
    connect(folder_toggle_expanded_action_, &QAction::triggered, this, [this] {
        const auto index = folder_view_->currentIndex();
        if (!index.isValid() || !folder_model_->isDirectory(index)) {
            return;
        }
        folder_view_->setExpanded(index, !folder_view_->isExpanded(index));
    });

    auto* workspace_menu = menuBar()->addMenu(QStringLiteral("&Workspace"));
    auto* commands = workspace_menu->addAction(tr("Commands…"));
    commands->setObjectName(QStringLiteral("action-command-palette"));
    commands->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
    connect(commands, &QAction::triggered, this, &BenchMainWindow::showCommandPalette);
    workspace_menu->addSeparator();
    auto* jump_playing = workspace_menu->addAction(tr("Jump to playing"));
    jump_playing->setObjectName(QStringLiteral("action-jump-to-playing"));
    jump_playing->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    connect(jump_playing, &QAction::triggered, this, [this] { refreshPlaybackCursor(true); });
    follow_playback_action_ = workspace_menu->addAction(tr("Cursor follows playback"));
    follow_playback_action_->setObjectName(QStringLiteral("action-follow-playback"));
    follow_playback_action_->setCheckable(true);
    follow_playback_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+J")));
    follow_playback_action_->setChecked(
        QSettings{}.value(QStringLiteral("workspace/follow-playback"), false).toBool());
    connect(follow_playback_action_, &QAction::toggled, this, [this](bool enabled) {
        QSettings{}.setValue(QStringLiteral("workspace/follow-playback"), enabled);
        followed_playback_index_ = QPersistentModelIndex{};
        refreshPlaybackCursor();
    });
    workspace_menu->addSeparator();
    auto* search_action = workspace_menu->addAction(QStringLiteral("Search…"));
    search_action->setObjectName(QStringLiteral("action-search-dialog"));
    search_action->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(search_action, &QAction::triggered, this, &BenchMainWindow::openSearchDialog);
    duplicate_tab_action_ = workspace_menu->addAction(QStringLiteral("Duplicate tab"));
    duplicate_tab_action_->setObjectName(QStringLiteral("action-duplicate-tab"));
    duplicate_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    connect(duplicate_tab_action_, &QAction::triggered, this,
            &BenchMainWindow::duplicateCurrentTab);
    pin_tab_action_ = workspace_menu->addAction(QStringLiteral("Pin tab"));
    pin_tab_action_->setObjectName(QStringLiteral("action-pin-tab"));
    pin_tab_action_->setCheckable(true);
    pin_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+P")));
    connect(pin_tab_action_, &QAction::triggered, this, &BenchMainWindow::toggleCurrentTabPinned);
    save_tab_action_ = workspace_menu->addAction(QStringLiteral("Save list"));
    save_tab_action_->setObjectName(QStringLiteral("action-save-list"));
    save_tab_action_->setShortcut(QKeySequence::Save);
    connect(save_tab_action_, &QAction::triggered, this, &BenchMainWindow::saveCurrentList);
    rename_tab_action_ = workspace_menu->addAction(QStringLiteral("Rename tab…"));
    rename_tab_action_->setObjectName(QStringLiteral("action-rename-tab"));
    rename_tab_action_->setShortcut(QKeySequence(Qt::Key_F2));
    connect(rename_tab_action_, &QAction::triggered, this, &BenchMainWindow::renameCurrentList);
    workspace_menu->addSeparator();
    close_tab_action_ = workspace_menu->addAction(QStringLiteral("Close tab"));
    close_tab_action_->setObjectName(QStringLiteral("action-close-tab"));
    close_tab_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(close_tab_action_, &QAction::triggered, this, &BenchMainWindow::closeCurrentTab);

    workspace_menu->addSeparator();
    auto* track_layout_menu = workspace_menu->addMenu(QStringLiteral("Track list layout"));
    track_layout_menu->setObjectName(QStringLiteral("menu-track-list-layout"));
    track_presentation_group_ = new QActionGroup(track_layout_menu);
    track_presentation_group_->setExclusive(true);
    track_albums_side_action_ =
        track_layout_menu->addAction(QStringLiteral("Albums with side artwork"));
    track_albums_side_action_->setObjectName(QStringLiteral("action-track-layout-albums-side"));
    track_albums_side_action_->setCheckable(true);
    track_presentation_group_->addAction(track_albums_side_action_);
    connect(track_albums_side_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::albums_side_artwork); });
    track_albums_header_action_ =
        track_layout_menu->addAction(QStringLiteral("Albums with header artwork"));
    track_albums_header_action_->setObjectName(QStringLiteral("action-track-layout-albums-header"));
    track_albums_header_action_->setCheckable(true);
    track_presentation_group_->addAction(track_albums_header_action_);
    connect(track_albums_header_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::albums_header_artwork); });
    track_plain_columns_action_ = track_layout_menu->addAction(QStringLiteral("Plain columns"));
    track_plain_columns_action_->setObjectName(QStringLiteral("action-track-layout-plain"));
    track_plain_columns_action_->setCheckable(true);
    track_presentation_group_->addAction(track_plain_columns_action_);
    connect(track_plain_columns_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::plain_columns); });
    track_compact_queue_action_ = track_layout_menu->addAction(QStringLiteral("Compact queue"));
    track_compact_queue_action_->setObjectName(QStringLiteral("action-track-layout-compact"));
    track_compact_queue_action_->setCheckable(true);
    track_presentation_group_->addAction(track_compact_queue_action_);
    connect(track_compact_queue_action_, &QAction::triggered, this,
            [this] { setTrackViewPresentation(ui::TrackViewPresentation::compact_queue); });
    track_layout_menu->addSeparator();
    track_columns_menu_ = track_layout_menu->addMenu(QStringLiteral("Columns"));
    track_columns_menu_->setObjectName(QStringLiteral("menu-track-columns"));
    for (const auto& spec : track_column_specs) {
        const auto id = QString::fromLatin1(spec.id);
        auto* action = track_columns_menu_->addAction(QString::fromLatin1(spec.label));
        action->setObjectName(QStringLiteral("action-track-column-%1").arg(id));
        action->setCheckable(true);
        connect(action, &QAction::toggled, this,
                [this, id](const bool visible) { setTrackColumnVisible(id, visible); });
        track_column_actions_.insert(id, action);
    }
    track_layout_reset_action_ =
        track_layout_menu->addAction(QStringLiteral("Reset current list layout"));
    track_layout_reset_action_->setObjectName(QStringLiteral("action-reset-track-layout"));
    connect(track_layout_reset_action_, &QAction::triggered, this,
            &BenchMainWindow::resetTrackViewLayout);
    track_layout_copy_action_ = track_layout_menu->addAction(
        QStringLiteral("Apply current layout to all queues and lists"));
    track_layout_copy_action_->setObjectName(QStringLiteral("action-copy-track-layout"));
    connect(track_layout_copy_action_, &QAction::triggered, this,
            &BenchMainWindow::copyTrackViewLayoutToAllTabs);

    workspace_menu->addSeparator();
    layout_edit_action_ = workspace_menu->addAction(QStringLiteral("Edit panel layout"));
    layout_edit_action_->setObjectName(QStringLiteral("action-edit-panel-layout"));
    layout_edit_action_->setCheckable(true);
    layout_edit_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    connect(layout_edit_action_, &QAction::toggled, this, &BenchMainWindow::setLayoutEditMode);
    auto* arrangement_menu = workspace_menu->addMenu(QStringLiteral("Panel arrangement"));
    arrangement_menu->setObjectName(QStringLiteral("menu-panel-arrangement"));
    layout_arrangement_group_ = new QActionGroup(arrangement_menu);
    layout_arrangement_group_->setExclusive(true);
    layout_side_by_side_action_ = arrangement_menu->addAction(QStringLiteral("Side by side"));
    layout_side_by_side_action_->setObjectName(QStringLiteral("action-layout-side-by-side"));
    layout_side_by_side_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_side_by_side_action_);
    connect(layout_side_by_side_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::split, Qt::Horizontal); });
    layout_top_bottom_action_ = arrangement_menu->addAction(QStringLiteral("Top and bottom"));
    layout_top_bottom_action_->setObjectName(QStringLiteral("action-layout-top-bottom"));
    layout_top_bottom_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_top_bottom_action_);
    connect(layout_top_bottom_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::split, Qt::Vertical); });
    layout_tabbed_action_ = arrangement_menu->addAction(QStringLiteral("Tabbed stack"));
    layout_tabbed_action_->setObjectName(QStringLiteral("action-layout-tabbed"));
    layout_tabbed_action_->setCheckable(true);
    layout_arrangement_group_->addAction(layout_tabbed_action_);
    connect(layout_tabbed_action_, &QAction::triggered, this,
            [this] { arrangePanelLayout(ui::PanelLayoutNodeKind::tabs, Qt::Horizontal); });
    layout_swap_action_ = workspace_menu->addAction(QStringLiteral("Swap panels"));
    layout_swap_action_->setObjectName(QStringLiteral("action-layout-swap-panels"));
    connect(layout_swap_action_, &QAction::triggered, this, &BenchMainWindow::swapPanelLayout);
    layout_reset_action_ = workspace_menu->addAction(QStringLiteral("Reset panel layout"));
    layout_reset_action_->setObjectName(QStringLiteral("action-reset-panel-layout"));
    connect(layout_reset_action_, &QAction::triggered, this, &BenchMainWindow::resetPanelLayout);

    tab_context_menu_ = new QMenu(tabs_);
    tab_context_menu_->setObjectName(QStringLiteral("bench-tab-context-menu"));
    tab_context_menu_->addAction(rename_tab_action_);
    tab_context_menu_->addAction(save_tab_action_);
    tab_context_menu_->addAction(pin_tab_action_);
    tab_context_menu_->addAction(duplicate_tab_action_);
    tab_context_menu_->addSeparator();
    tab_context_menu_->addAction(close_tab_action_);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showTabContextMenu);
    track_context_menu_ = new QMenu(tabs_);
    track_context_menu_->setObjectName(QStringLiteral("bench-track-context-menu"));
    folder_context_menu_ = new QMenu(folders_panel_);
    folder_context_menu_->setObjectName(QStringLiteral("bench-folder-context-menu"));
    refreshTabActions();
    refreshTrackViewActions();
    refreshPanelLayoutActions();
}

ui::PanelLayout BenchMainWindow::defaultPanelLayout() const {
    std::vector<ui::PanelLayoutNode> children;
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(folders_panel_id)));
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(track_lists_panel_id)));
    return ui::PanelLayout{
        .schema_version = ui::panel_layout_schema_version,
        .root = ui::panelLayoutSplit(Qt::Horizontal, std::move(children), {1, 3}),
    };
}

void BenchMainWindow::loadPanelLayout() {
    QSettings settings;
    const auto encoded =
        settings.value(QString::fromLatin1(panel_layout_settings_key)).toByteArray();
    if (encoded.isEmpty()) {
        applyPanelLayout(defaultPanelLayout());
        return;
    }

    QString error;
    const QStringList registered_panel_ids = panel_widgets_.keys();
    auto restored = ui::deserializePanelLayout(encoded, registered_panel_ids, &error);
    if (!restored) {
        panel_layout_persistence_protected_ = true;
        applyPanelLayout(defaultPanelLayout());
        statusBar()->showMessage(
            QStringLiteral("Panel layout was not loaded (%1); the saved value was preserved")
                .arg(error),
            7'000);
        return;
    }
    applyPanelLayout(*restored);
}

void BenchMainWindow::applyPanelLayout(const ui::PanelLayout& layout) {
    applying_panel_layout_ = true;
    auto* previous_root = layout_root_;
    for (auto* panel : panel_widgets_) {
        panel->hide();
        panel->setParent(layout_host_);
    }
    if (previous_root != nullptr) {
        layout_host_layout_->removeWidget(previous_root);
    }
    layout_root_ = renderPanelLayoutNode(layout.root, layout_host_);
    layout_host_layout_->addWidget(layout_root_);
    layout_root_->show();
    for (auto* panel : panel_widgets_) {
        panel->show();
    }
    if (previous_root != nullptr && !panel_widgets_.values().contains(previous_root)) {
        previous_root->deleteLater();
    }
    applying_panel_layout_ = false;
    refreshPanelLayoutActions();
}

QWidget* BenchMainWindow::renderPanelLayoutNode(const ui::PanelLayoutNode& node, QWidget* parent) {
    if (node.kind == ui::PanelLayoutNodeKind::panel) {
        auto* panel = panel_widgets_.value(node.panel_id, nullptr);
        Q_ASSERT(panel != nullptr);
        panel->setParent(parent);
        return panel;
    }
    if (node.kind == ui::PanelLayoutNodeKind::split) {
        auto* splitter = new QSplitter(node.orientation, parent);
        splitter->setObjectName(QStringLiteral("bench-panel-layout-split"));
        splitter->setProperty(layout_container_kind_property, QStringLiteral("split"));
        splitter->setChildrenCollapsible(false);
        for (const auto& child : node.children) {
            splitter->addWidget(renderPanelLayoutNode(child, splitter));
        }
        QList<int> sizes;
        sizes.reserve(static_cast<qsizetype>(node.weights.size()));
        for (int index = 0; const auto weight : node.weights) {
            sizes.push_back(weight);
            splitter->setStretchFactor(index++, weight);
        }
        splitter->setSizes(sizes);
        QTimer::singleShot(0, splitter, [splitter, weights = node.weights] {
            const auto extent =
                splitter->orientation() == Qt::Horizontal ? splitter->width() : splitter->height();
            int total = 0;
            for (const auto weight : weights) {
                total += weight;
            }
            total = std::max(1, total);
            QList<int> scaled;
            scaled.reserve(static_cast<qsizetype>(weights.size()));
            for (const auto weight : weights) {
                scaled.push_back(std::max(1, extent * weight / total));
            }
            splitter->setSizes(scaled);
        });
        connect(splitter, &QSplitter::splitterMoved, this, [this](const int, const int) {
            if (applying_panel_layout_) {
                return;
            }
            panel_layout_persistence_protected_ = false;
            persistPanelLayout();
        });
        return splitter;
    }

    auto* stack = new QTabWidget(parent);
    stack->setObjectName(QStringLiteral("bench-panel-layout-tabs"));
    stack->setProperty(layout_container_kind_property, QStringLiteral("tabs"));
    stack->setDocumentMode(true);
    stack->setMovable(true);
    for (const auto& child : node.children) {
        auto* child_widget = renderPanelLayoutNode(child, stack);
        auto title = child_widget->property(layout_panel_title_property).toString();
        if (title.isEmpty()) {
            title = QStringLiteral("Panel group");
        }
        stack->addTab(child_widget, title);
    }
    stack->setCurrentIndex(node.active_child);
    connect(stack, &QTabWidget::currentChanged, this, [this](const int) {
        if (!applying_panel_layout_) {
            persistPanelLayout();
        }
    });
    connect(stack->tabBar(), &QTabBar::tabMoved, this, [this](const int, const int) {
        if (!applying_panel_layout_) {
            panel_layout_persistence_protected_ = false;
            persistPanelLayout();
        }
    });
    return stack;
}

ui::PanelLayoutNode BenchMainWindow::capturePanelLayoutNode(QWidget* widget) const {
    const auto panel_id = widget->property(layout_panel_id_property).toString();
    if (!panel_id.isEmpty()) {
        return ui::panelLayoutPanel(panel_id);
    }
    const auto container_kind = widget->property(layout_container_kind_property).toString();
    if (container_kind == QStringLiteral("split")) {
        auto* splitter = qobject_cast<QSplitter*>(widget);
        Q_ASSERT(splitter != nullptr);
        std::vector<ui::PanelLayoutNode> children;
        children.reserve(static_cast<std::size_t>(splitter->count()));
        for (int index = 0; index < splitter->count(); ++index) {
            children.push_back(capturePanelLayoutNode(splitter->widget(index)));
        }
        std::vector<int> weights;
        const auto sizes = splitter->sizes();
        weights.reserve(static_cast<std::size_t>(sizes.size()));
        for (const auto size : sizes) {
            weights.push_back(std::max(size, 1));
        }
        return ui::panelLayoutSplit(splitter->orientation(), std::move(children),
                                    std::move(weights));
    }
    if (container_kind == QStringLiteral("tabs")) {
        auto* stack = qobject_cast<QTabWidget*>(widget);
        Q_ASSERT(stack != nullptr);
        std::vector<ui::PanelLayoutNode> children;
        children.reserve(static_cast<std::size_t>(stack->count()));
        for (int index = 0; index < stack->count(); ++index) {
            children.push_back(capturePanelLayoutNode(stack->widget(index)));
        }
        return ui::panelLayoutTabs(std::move(children), stack->currentIndex());
    }
    Q_ASSERT_X(false, "BenchMainWindow::capturePanelLayoutNode",
               "panel-layout renderer produced an unknown widget");
    return ui::panelLayoutPanel(QStringLiteral("invalid"));
}

void BenchMainWindow::persistPanelLayout() {
    if (layout_root_ == nullptr || applying_panel_layout_ || panel_layout_persistence_protected_) {
        return;
    }
    const ui::PanelLayout layout{.schema_version = ui::panel_layout_schema_version,
                                 .root = capturePanelLayoutNode(layout_root_)};
    QSettings settings;
    settings.setValue(QString::fromLatin1(panel_layout_settings_key),
                      ui::serializePanelLayout(layout));
}

void BenchMainWindow::setLayoutEditMode(const bool editing) {
    layout_host_->setProperty("trackknifeLayoutEditing", editing);
    layout_host_->setStyleSheet(editing
                                    ? QStringLiteral("QWidget[trackknifeLayoutPanel=\"true\"] {"
                                                     " border: 1px dashed palette(highlight); }")
                                    : QString{});
    if (editing) {
        statusBar()->showMessage(
            QStringLiteral("Panel layout editing: choose an arrangement or swap panels"));
    } else {
        statusBar()->clearMessage();
    }
    refreshPanelLayoutActions();
}

void BenchMainWindow::arrangePanelLayout(const ui::PanelLayoutNodeKind kind,
                                         const Qt::Orientation orientation) {
    if (layout_root_ == nullptr || kind == ui::PanelLayoutNodeKind::panel) {
        return;
    }
    auto root = capturePanelLayoutNode(layout_root_);
    if (root.children.empty()) {
        return;
    }
    auto children = std::move(root.children);
    ui::PanelLayoutNode replacement;
    if (kind == ui::PanelLayoutNodeKind::split) {
        auto weights = root.kind == ui::PanelLayoutNodeKind::split
                           ? std::move(root.weights)
                           : std::vector<int>(children.size(), 1);
        replacement = ui::panelLayoutSplit(orientation, std::move(children), std::move(weights));
    } else {
        auto active = root.kind == ui::PanelLayoutNodeKind::tabs ? root.active_child : 0;
        if (root.kind != ui::PanelLayoutNodeKind::tabs) {
            const auto track_lists =
                std::ranges::find(children, QString::fromLatin1(track_lists_panel_id),
                                  &ui::PanelLayoutNode::panel_id);
            if (track_lists != children.end()) {
                active = static_cast<int>(std::distance(children.begin(), track_lists));
            }
        }
        replacement = ui::panelLayoutTabs(std::move(children), active);
    }
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                                     .root = std::move(replacement)});
    persistPanelLayout();
}

void BenchMainWindow::swapPanelLayout() {
    if (layout_root_ == nullptr) {
        return;
    }
    auto root = capturePanelLayoutNode(layout_root_);
    if (root.children.size() < 2U) {
        return;
    }
    std::ranges::reverse(root.children);
    if (root.kind == ui::PanelLayoutNodeKind::split) {
        std::ranges::reverse(root.weights);
    } else if (root.kind == ui::PanelLayoutNodeKind::tabs) {
        root.active_child = static_cast<int>(root.children.size()) - 1 - root.active_child;
    }
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                                     .root = std::move(root)});
    persistPanelLayout();
}

void BenchMainWindow::resetPanelLayout() {
    panel_layout_persistence_protected_ = false;
    applyPanelLayout(defaultPanelLayout());
    persistPanelLayout();
}

void BenchMainWindow::refreshPanelLayoutActions() {
    if (layout_edit_action_ == nullptr) {
        return;
    }
    const bool editing = layout_edit_action_->isChecked();
    for (auto* action : {layout_side_by_side_action_, layout_top_bottom_action_,
                         layout_tabbed_action_, layout_swap_action_}) {
        action->setEnabled(editing && layout_root_ != nullptr);
    }
    layout_reset_action_->setEnabled(layout_root_ != nullptr);
    if (layout_root_ == nullptr) {
        return;
    }
    const auto root = capturePanelLayoutNode(layout_root_);
    const QSignalBlocker horizontal_blocker{layout_side_by_side_action_};
    const QSignalBlocker vertical_blocker{layout_top_bottom_action_};
    const QSignalBlocker tabbed_blocker{layout_tabbed_action_};
    layout_side_by_side_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::split &&
                                            root.orientation == Qt::Horizontal);
    layout_top_bottom_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::split &&
                                          root.orientation == Qt::Vertical);
    layout_tabbed_action_->setChecked(root.kind == ui::PanelLayoutNodeKind::tabs);
}

void BenchMainWindow::loadFolderBookmarks() {
    QSettings settings;
    auto stored = settings.value(QStringLiteral("library/bookmarks")).toList();
    if (!settings.contains(QStringLiteral("library/bookmarks"))) {
        // First run of the bookmark panel: the old manually added library
        // roots become bookmarks, headed by the home directory.
        stored.push_back(QFile::encodeName(QDir::homePath()));
        for (const auto& root : settings.value(QStringLiteral("library/roots")).toList()) {
            if (!root.toByteArray().isEmpty()) {
                stored.push_back(root);
            }
        }
        settings.setValue(QStringLiteral("library/bookmarks"), stored);
    }
    folder_bookmarks_->clear();
    for (const auto& entry : stored) {
        const auto bytes = entry.toByteArray();
        if (bytes.isEmpty()) {
            continue;
        }
        const std::string raw_path{bytes.constData(), static_cast<std::size_t>(bytes.size())};
        const auto display = QString::fromUtf8(
            core::escape_raw_path(std::filesystem::path{raw_path}.filename().native().empty()
                                      ? raw_path
                                      : std::filesystem::path{raw_path}.filename().native()));
        auto* item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("folder")), display,
                                         folder_bookmarks_);
        item->setToolTip(QString::fromUtf8(core::escape_raw_path(raw_path)));
        item->setData(Qt::UserRole, bytes);
    }
    folder_bookmarks_->setVisible(folder_bookmarks_->count() > 0);
    folder_bookmarks_heading_->setVisible(folder_bookmarks_->isVisibleTo(folders_panel_));
}

void BenchMainWindow::persistFolderBookmarks() const {
    QSettings settings;
    QVariantList stored;
    for (int row = 0; row < folder_bookmarks_->count(); ++row) {
        stored.push_back(folder_bookmarks_->item(row)->data(Qt::UserRole));
    }
    settings.setValue(QStringLiteral("library/bookmarks"), stored);
}

void BenchMainWindow::addFolderBookmark(const std::string& raw_path) {
    const QByteArray bytes{raw_path.data(), static_cast<qsizetype>(raw_path.size())};
    for (int row = 0; row < folder_bookmarks_->count(); ++row) {
        if (folder_bookmarks_->item(row)->data(Qt::UserRole).toByteArray() == bytes) {
            return;
        }
    }
    const auto name = std::filesystem::path{raw_path}.filename().native();
    auto* item = new QListWidgetItem(
        QIcon::fromTheme(QStringLiteral("folder")),
        QString::fromUtf8(core::escape_raw_path(name.empty() ? raw_path : name)),
        folder_bookmarks_);
    item->setToolTip(QString::fromUtf8(core::escape_raw_path(raw_path)));
    item->setData(Qt::UserRole, bytes);
    persistFolderBookmarks();
    folder_bookmarks_->setVisible(true);
    folder_bookmarks_heading_->setVisible(folder_bookmarks_->isVisibleTo(folders_panel_));
}

void BenchMainWindow::showFolderBookmarkMenu(const QPoint& position) {
    const auto index = folder_bookmarks_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    folder_bookmarks_->setCurrentRow(index.row());
    folder_bookmark_menu_->popup(folder_bookmarks_->viewport()->mapToGlobal(position));
}

// Reveals a bookmarked directory in the lazy tree: walk the path from its
// root, fetching one level at a time and continuing when the rows arrive.
void BenchMainWindow::revealFolderPath(const std::string& raw_path) {
    for (int row = 0; row < folder_model_->rowCount(); ++row) {
        const auto root_index = folder_model_->index(row, 0);
        const auto root_path = folder_model_->rawPath(root_index);
        if (raw_path == root_path) {
            folder_view_->setCurrentIndex(root_index);
            folder_view_->scrollTo(root_index);
            folder_view_->expand(root_index);
            return;
        }
        const auto prefix = root_path == "/" ? std::string{"/"} : root_path + '/';
        if (raw_path.starts_with(prefix)) {
            revealFolderStep(QPersistentModelIndex{root_index}, raw_path);
            return;
        }
    }
    // Not under any library root yet: the bookmark becomes a root.
    folder_model_->addRoot(raw_path);
    QSettings settings;
    auto roots = settings.value(QStringLiteral("library/roots")).toList();
    roots.push_back(QByteArray{raw_path.data(), static_cast<qsizetype>(raw_path.size())});
    settings.setValue(QStringLiteral("library/roots"), roots);
    for (int row = 0; row < folder_model_->rowCount(); ++row) {
        const auto root_index = folder_model_->index(row, 0);
        if (folder_model_->rawPath(root_index) == raw_path) {
            folder_view_->setCurrentIndex(root_index);
            folder_view_->scrollTo(root_index);
            return;
        }
    }
}

void BenchMainWindow::revealFolderStep(const QPersistentModelIndex& parent_index,
                                       const std::string& raw_path) {
    if (!parent_index.isValid()) {
        return;
    }
    const QModelIndex parent{parent_index};
    if (!folder_model_->isLoaded(parent)) {
        // Waits for the listing whether this starts it or another reveal
        // already has. Treating an in-flight listing as loaded -- which is
        // what asking canFetchMore did -- found no children and gave up, so a
        // bookmark clicked while the startup reveal was still listing "/"
        // silently did nothing.
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(folder_model_, &ui::LocalFolderTreeModel::directoryLoaded, this,
                    [this, connection, parent_index, raw_path](const QModelIndex& loaded) {
                        if (loaded != QModelIndex{parent_index}) {
                            return;
                        }
                        disconnect(*connection);
                        revealFolderStep(parent_index, raw_path);
                    });
        if (folder_model_->canFetchMore(parent)) {
            folder_model_->fetchMore(parent);
        }
        return;
    }
    folder_view_->expand(parent);
    for (int row = 0; row < folder_model_->rowCount(parent); ++row) {
        const auto child = folder_model_->index(row, 0, parent);
        const auto child_path = folder_model_->rawPath(child);
        if (child_path == raw_path) {
            folder_view_->setCurrentIndex(child);
            folder_view_->scrollTo(child);
            return;
        }
        if (raw_path.starts_with(child_path + '/')) {
            revealFolderStep(QPersistentModelIndex{child}, raw_path);
            return;
        }
    }
}

} // namespace trackknife::bench

void trackknife::bench::BenchMainWindow::showCommandPalette() {
    for (auto* existing : findChildren<ui::CommandPalette*>()) {
        if (existing->isVisible()) {
            existing->raise();
            existing->activateWindow();
            return;
        }
    }
    QList<QAction*> commands;
    // Deliberate task inventory. Device names, rating values, column names and
    // transient context-menu choices are parameters, not standalone commands.
    for (const auto* id : {"action-open-files",       "action-open-folder",
                           "action-new-list",         "action-import-m3u8",
                           "action-export-m3u8",      "action-dynamic-playlists",
                           "action-settings",         "action-search-dialog",
                           "action-find-in-list",     "action-jump-to-playing",
                           "action-follow-playback",  "action-show-up-next",
                           "action-play-pause",       "action-stop",
                           "action-next-track",       "action-previous-track",
                           "action-save-list",        "action-rename-tab",
                           "action-duplicate-tab",    "action-close-tab",
                           "action-track-properties", "action-replaygain-dialog",
                           "action-convert-files",    "action-reverse-list",
                           "action-shuffle-albums",   "action-local-album-random",
                           "action-deduplicate-list", "action-backup-workspace",
                           "action-restore-workspace"}) {
        if (auto* action = findChild<QAction*>(QString::fromLatin1(id)))
            commands.append(action);
    }
    auto* palette = new ui::CommandPalette(std::move(commands), this);
    palette->setAttribute(Qt::WA_DeleteOnClose);
    palette->open();
}

trackknife::bench::SettingsDialog*
trackknife::bench::BenchMainWindow::showSettingsDialog(const SettingsDialog::Page page) {
    for (auto* existing : findChildren<SettingsDialog*>()) {
        if (existing->isVisible()) {
            existing->showPage(page);
            existing->raise();
            existing->activateWindow();
            return existing;
        }
    }
    std::function<QWidget*(QWidget*)> library_folders;
    if (local_library_) {
        library_folders = [this](QWidget* parent) {
            return local_library_->createFoldersWidget(parent);
        };
    }
    auto* dialog = new SettingsDialog(
        this, buildOutputProfileStore(), std::move(library_folders),
        [this](QWidget* parent) { return buildLastFmSettings(parent); }, configurable_shortcuts_);
    // ADR-0185: profile edits in Settings refresh every open tag editor's
    // selectors immediately.
    connect(dialog, &SettingsDialog::outputProfilesChanged, this, [this] {
        for (int index = 0; index < tabs_->count(); ++index) {
            if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
                properties->reloadOutputProfiles();
            }
        }
    });
    connect(dialog, &QDialog::accepted, this, [this] { applyLocalLibraryVisibility(); });
    connect(dialog, &QDialog::accepted, this, [this, sharing = localEngineSharing()] {
        // ADR-0226: this computer's engine runs apart from the window, so a
        // change to how it is shared means starting it again.
        if (localEngineSharing() != sharing && catalogue_source_) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool restarted = catalogue_source_->restartLocalEngine();
            QApplication::restoreOverrideCursor();
            statusBar()->showMessage(
                restarted ? QStringLiteral("This computer's engine restarted with its new settings")
                          : QStringLiteral("This computer's engine did not restart; see its log"),
                8'000);
        }
        reloadPlaybackPreferences();
        for (auto* section : findChildren<MetadataArtworkSection*>())
            section->refreshStoragePolicy();
    });
    dialog->showPage(page);
    dialog->open();
    return dialog;
}
