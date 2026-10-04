// SPDX-License-Identifier: GPL-3.0-only

#include "bench/undo_location_widget.hpp"
#include "workspace/undo_location_session.hpp"
#include "bench/bench_main_window.hpp"

#include "workspace/panel_arrangement.hpp"
#include "bench/engine_launcher.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/metadata_artwork_section.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/quick_pick_popup.hpp"
#include "bench/trackknife_style.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "workspace/sources.hpp"
#include <QRandomGenerator>
#include <QStyle>

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
#include <QtConcurrent/QtConcurrentRun>
#include <filesystem>
#include <memory>

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {} // namespace

namespace {

constexpr auto folders_panel_id = PanelArrangement::sources_panel;
constexpr auto track_lists_panel_id = PanelArrangement::tracks_panel;
constexpr auto layout_panel_id_property = "trackknife-layout-panel-id";
constexpr auto layout_panel_title_property = "trackknife-layout-panel-title";
constexpr auto layout_container_kind_property = "trackknife-layout-container-kind";

} // namespace

void BenchMainWindow::buildWorkspace() {
    tabs_ = new PlaybackTabWidget(this);
    tabs_->setObjectName(QStringLiteral("bench-tabs"));
    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
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
        refreshListsPanel();
        refreshTabActions();
        refreshTrackViewActions();
        refreshSelectionStatus();
        refreshActiveContext();
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, [this](const int, const int) {
        keepTabGroupsTogether();
        schedulePersist();
    });
    static_cast<PlaybackTabWidget*>(tabs_)->tabs_changed = [this] { refreshListsPanel(); };
    // The tracks, and beside them the lists when they are shown as a pane.
    track_area_ = new QSplitter(Qt::Horizontal, this);
    track_area_->setObjectName(QStringLiteral("bench-track-area"));
    track_area_->setChildrenCollapsible(false);
    track_area_->setProperty(layout_panel_id_property, QString::fromLatin1(track_lists_panel_id));
    track_area_->setProperty(layout_panel_title_property, QStringLiteral("Track Lists"));
    track_area_->setProperty("trackknifeLayoutPanel", true);
    track_area_->addWidget(tabs_);
    buildListsPanel();

    folder_browser_ = new FolderBrowser(this);
    folder_model_ = folder_browser_->model();
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
    heading_row->setContentsMargins(6, 6, 6, 4);
    heading_row->setSpacing(2);
    // One-click source switching (ADR-0130): a flat tab bar per authority
    // replaces the dropdown and static heading.
    const auto make_source_tabs = [this](const QString& object_name,
                                         const QString& accessible_name) {
        // The same tabs as the lists', without close buttons: a source is
        // chosen, not closed.
        auto* bar = new PlaybackTabBar(folders_panel_, false);
        bar->setDrawBase(false);
        bar->setExpanding(false);
        bar->setUsesScrollButtons(true);
        bar->setElideMode(Qt::ElideRight);
        bar->setObjectName(object_name);
        bar->setAccessibleName(accessible_name);
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
        rememberSource(!kind.isEmpty() ? QStringLiteral("remote")
                       : index == 0    ? QStringLiteral("folders")
                       : index == 1    ? QStringLiteral("library")
                                       : QString{});
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
    // Folders and files dragged into a list, as from the library.
    folder_view_->setDragEnabled(true);
    folder_view_->setDragDropMode(QAbstractItemView::DragOnly);
    folder_view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    folder_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folder_view_, &QWidget::customContextMenuRequested, this,
            &BenchMainWindow::showFolderContextMenu);
    connect(folder_browser_, &FolderBrowser::expandRequested, folder_view_,
            [this](const QModelIndex& index) { folder_view_->expand(index); });
    connect(folder_browser_, &FolderBrowser::currentRequested, folder_view_,
            [this](const QModelIndex& index) {
                folder_view_->setCurrentIndex(index);
                folder_view_->scrollTo(index);
            });
    connect(folder_browser_, &FolderBrowser::bookmarksChanged, this,
            &BenchMainWindow::loadFolderBookmarks);
    connect(folder_view_, &QTreeView::activated, this, [this](const QModelIndex& index) {
        if (!index.isValid() || folder_model_->isDirectory(index)) {
            return;
        }
        openLocalPaths({folder_model_->rawPath(index)});
    });
    source_stack_->addWidget(folder_view_);
    folders_layout->addWidget(source_stack_, 1);

    panel_widgets_.insert(QString::fromLatin1(folders_panel_id), folders_panel_);
    panel_widgets_.insert(QString::fromLatin1(track_lists_panel_id), track_area_);
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
    connect(folder_bookmark_remove_action_, &QAction::triggered, this,
            [this] { folder_browser_->removeBookmark(folder_bookmarks_->currentRow()); });
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
    // ADR-0233: the lists of every engine this window reaches.
    auto* open_list = file_menu->addAction(QStringLiteral("Open list…"));
    open_list->setObjectName(QStringLiteral("action-open-list"));
    open_list->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+O")));
    connect(open_list, &QAction::triggered, this, &BenchMainWindow::showOpenListDialog);
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
    // The two ways out, side by side and named for what they do to the
    // music: only a tooltip told them apart, and menus do not show those.
    auto* close_window = file_menu->addAction(QStringLiteral("Close window"));
    close_window->setObjectName(QStringLiteral("action-close-window"));
    close_window->setStatusTip(QStringLiteral("The music keeps playing"));
    connect(close_window, &QAction::triggered, this, &QWidget::close);
    auto* quit = file_menu->addAction(QStringLiteral("Quit and stop playback"));
    quit->setObjectName(QStringLiteral("action-quit"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setStatusTip(QStringLiteral("Close Trackknife and stop this computer's engine"));
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
    identify_albums_action_ = edit_menu->addAction(QStringLiteral("Identify albums…"));
    identify_albums_action_->setObjectName(QStringLiteral("action-identify-albums"));
    identify_albums_action_->setToolTip(
        QStringLiteral("Look up the selected tracks' albums on MusicBrainz -- the whole list "
                       "when none are selected -- and write them"));
    identify_albums_action_->setEnabled(false);
    connect(identify_albums_action_, &QAction::triggered, this,
            &BenchMainWindow::showIdentifyAlbums);
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
    auto* quick_album = workspace_menu->addAction(tr("Quick album…"));
    quick_album->setObjectName(QStringLiteral("action-quick-album"));
    quick_album->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    quick_album->setShortcutContext(Qt::WindowShortcut);
    connect(quick_album, &QAction::triggered, this,
            [this] { openQuickPick(QuickPickKind::album); });
    auto* quick_track = workspace_menu->addAction(tr("Quick track…"));
    quick_track->setObjectName(QStringLiteral("action-quick-track"));
    quick_track->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    quick_track->setShortcutContext(Qt::WindowShortcut);
    connect(quick_track, &QAction::triggered, this,
            [this] { openQuickPick(QuickPickKind::track); });
    // ADR-0233: a tab bar, or a pane that keeps every list in sight.
    lists_panel_action_ = workspace_menu->addAction(tr("Lists in a side panel"));
    lists_panel_action_->setObjectName(QStringLiteral("action-lists-panel"));
    lists_panel_action_->setCheckable(true);
    lists_panel_action_->setChecked(listsInPanel());
    connect(lists_panel_action_, &QAction::toggled, this, [this](const bool panel) {
        QSettings{}.setValue(QLatin1String(lists_display_key),
                             panel ? QStringLiteral("panel") : QStringLiteral("tabs"));
        applyListsDisplay();
    });
    workspace_menu->addSeparator();
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
    continue_menu_ = tab_context_menu_->addMenu(tr("Continue with"));
    continue_menu_->setObjectName(QStringLiteral("bench-tab-continue-menu"));
    continue_menu_->setToolTip(
        tr("When the list would end, add tracks from a dynamic playlist's rule"));
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
    return PanelArrangement::defaultLayout();
}

void BenchMainWindow::loadPanelLayout() {
    if (panel_arrangement_ == nullptr) {
        panel_arrangement_ = new PanelArrangement(this);
    }
    const auto error = panel_arrangement_->load();
    applyPanelLayout(panel_arrangement_->layout());
    if (!error.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Panel layout was not loaded (%1); the saved value was preserved")
                .arg(error),
            7'000);
    }
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
            if (!applying_panel_layout_) {
                persistPanelLayout(true);
            }
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
            persistPanelLayout(false);
        }
    });
    connect(stack->tabBar(), &QTabBar::tabMoved, this, [this](const int, const int) {
        if (!applying_panel_layout_) {
            persistPanelLayout(true);
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

void BenchMainWindow::persistPanelLayout(const bool by_hand) {
    if (layout_root_ == nullptr || applying_panel_layout_ || panel_arrangement_ == nullptr) {
        return;
    }
    panel_arrangement_->adopt(ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                                              .root = capturePanelLayoutNode(layout_root_)},
                              by_hand);
}

void BenchMainWindow::setLayoutEditMode(const bool editing) {
    if (panel_arrangement_ != nullptr) {
        panel_arrangement_->setEditing(editing);
    }
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
    if (layout_root_ == nullptr || panel_arrangement_ == nullptr) {
        return;
    }
    // As it is now, sizes included, then arranged anew.
    persistPanelLayout(false);
    panel_arrangement_->arrange(kind, orientation);
    applyPanelLayout(panel_arrangement_->layout());
}

void BenchMainWindow::swapPanelLayout() {
    if (layout_root_ == nullptr || panel_arrangement_ == nullptr) {
        return;
    }
    persistPanelLayout(false);
    panel_arrangement_->swap();
    applyPanelLayout(panel_arrangement_->layout());
}

void BenchMainWindow::resetPanelLayout() {
    if (panel_arrangement_ == nullptr) {
        return;
    }
    panel_arrangement_->reset();
    applyPanelLayout(panel_arrangement_->layout());
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
    folder_bookmarks_->clear();
    for (const auto& raw_path : folder_browser_->bookmarkPaths()) {
        auto* item = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("folder"), style()->standardIcon(QStyle::SP_DirIcon)),
            folderBookmarkLabel(raw_path), folder_bookmarks_);
        item->setToolTip(folderBookmarkTooltip(raw_path));
        item->setData(Qt::UserRole,
                      QByteArray{raw_path.data(), static_cast<qsizetype>(raw_path.size())});
    }
    const auto folders_visible =
        local_source_tabs_ == nullptr || local_source_tabs_->currentIndex() == 0;
    folder_bookmarks_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
    folder_bookmarks_heading_->setVisible(folders_visible && folder_bookmarks_->count() > 0);
}

void BenchMainWindow::showFolderBookmarkMenu(const QPoint& position) {
    const auto index = folder_bookmarks_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    folder_bookmarks_->setCurrentRow(index.row());
    folder_bookmark_menu_->popup(folder_bookmarks_->viewport()->mapToGlobal(position));
}

} // namespace trackknife::bench

void trackknife::bench::BenchMainWindow::openQuickPick(const QuickPickKind kind) {
    // The library of the tab in front: a remote tab's albums come from its
    // engine and go where that tab's would.
    const auto* current = currentListTab();
    auto engine = current != nullptr ? EngineKey::of(current->document) : EngineKey::local();
    if (catalogueOf(engine) == nullptr || libraryOf(engine) == nullptr) {
        engine = EngineKey::local();
    }
    auto* library = libraryOf(engine);
    const auto* source = catalogueOf(engine);
    if (library == nullptr || source == nullptr) {
        return;
    }
    auto* popup = new QuickPickPopup(kind, std::shared_ptr<engine::Catalogue>{source->open()},
                                     source->name(), this);
    connect(popup, &QuickPickPopup::chosen, library,
            [library](std::vector<persistence::LibraryEntry> picked, LocalLibraryAction action) {
                // Exactly what the library's own menu does with it.
                emit library->browser().actionRequested(std::move(picked), action);
            });
    popup->popUp(centralWidget() != nullptr ? centralWidget() : this);
}

void trackknife::bench::BenchMainWindow::showCommandPalette() {
    for (auto* existing : findChildren<ui::CommandPalette*>()) {
        if (existing->isVisible()) {
            existing->raise();
            existing->activateWindow();
            return;
        }
    }
    QList<QAction*> commands;
    for (const auto* id : workspace_command_ids) {
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
    // Every engine's music folders, one at a time: the same folders its
    // library's Folders… opens.
    std::function<QWidget*(QWidget*)> library_folders;
    if (std::ranges::any_of(engines_, [](const auto& engine) { return engine->library != nullptr; })) {
        library_folders = [this](QWidget* parent) -> QWidget* {
            auto* widget = new QWidget(parent);
            auto* layout = new QVBoxLayout(widget);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(tr("Engine:"), widget);
            auto* picker = new QComboBox(widget);
            picker->setObjectName(QStringLiteral("bench-settings-library-engine"));
            picker->setAccessibleName(tr("Engine whose music folders are shown"));
            row->addWidget(label);
            row->addWidget(picker);
            row->addStretch(1);
            layout->addLayout(row);
            auto* folders = new QStackedWidget(widget);
            for (const auto& engine : engines_) {
                if (engine->library == nullptr) {
                    continue;
                }
                picker->addItem(engine->key.isLocal() ? tr("This computer")
                                                      : engineName(engine->key),
                                engine->key.text());
                folders->addWidget(engine->library->createFoldersWidget(folders));
            }
            // One engine: nothing to choose.
            label->setVisible(picker->count() > 1);
            picker->setVisible(picker->count() > 1);
            connect(picker, &QComboBox::currentIndexChanged, folders,
                    &QStackedWidget::setCurrentIndex);
            layout->addWidget(folders, 1);
            return widget;
        };
    }
    // ADR-0266: where each engine keeps its undo copies, asked of it.
    std::function<QWidget*(QWidget*)> undo_locations;
    if (std::ranges::any_of(engines_, [](const auto& engine) { return engine->file_work != nullptr; })) {
        undo_locations = [this](QWidget* parent) -> QWidget* {
            auto* widget = new QWidget(parent);
            auto* layout = new QVBoxLayout(widget);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(tr("Engine:"), widget);
            auto* picker = new QComboBox(widget);
            picker->setObjectName(QStringLiteral("bench-settings-undo-engine"));
            picker->setAccessibleName(tr("Engine whose undo copies are shown"));
            row->addWidget(label);
            row->addWidget(picker);
            row->addStretch(1);
            layout->addLayout(row);
            auto* places = new QStackedWidget(widget);
            for (const auto& engine : engines_) {
                if (engine->file_work == nullptr) {
                    continue;
                }
                const auto name =
                    engine->key.isLocal() ? tr("This computer") : engineName(engine->key);
                picker->addItem(name, engine->key.text());
                auto* session = new UndoLocationSession(
                    engine->file_work,
                    engine->key.isLocal() ? EngineFolderLister{}
                                          : workspace_.engineFolderLister(engine->file_work),
                    places);
                places->addWidget(new UndoLocationWidget(*session, name, places));
            }
            label->setVisible(picker->count() > 1);
            picker->setVisible(picker->count() > 1);
            connect(picker, &QComboBox::currentIndexChanged, places,
                    &QStackedWidget::setCurrentIndex);
            layout->addWidget(places);
            return widget;
        };
    }
    auto* dialog = new SettingsDialog(
        this, buildOutputProfileStore(), std::move(library_folders),
        [this](QWidget* parent) { return buildLastFmSettings(parent); }, configurable_shortcuts_,
        std::move(undo_locations));
    // ADR-0185: profile edits in Settings refresh every open tag editor's
    // selectors immediately.
    connect(dialog, &SettingsDialog::outputProfilesChanged, this, [this] {
        // ADR-0221: tag editors are windows of this one, not tabs.
        for (auto* properties : findChildren<MetadataPropertiesDialog*>()) {
            properties->reloadOutputProfiles();
        }
    });
    connect(dialog, &QDialog::accepted, this, [this, sharing = localEngineSharing()] {
        workspace_.settingsSaved(sharing);
        applyLocalLibraryVisibility();
        applyListsDisplay();
        if (notifications_action_)
            notifications_action_->setChecked(
                QSettings{}.value(QStringLiteral("desktop/notifications"), false).toBool());
        for (auto* section : findChildren<MetadataArtworkSection*>())
            section->refreshStoragePolicy();
    });
    dialog->showPage(page);
    dialog->open();
    return dialog;
}
