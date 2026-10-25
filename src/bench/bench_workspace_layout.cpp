// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/lists_panel.hpp"
#include "bench/playlist_folder_widget.hpp"
#include "bench/themed_icon.hpp"
#include "bench/undo_location_widget.hpp"
#include "workspace/list_view_definitions.hpp"
#include "workspace/library_watch_session.hpp"
#include "workspace/playlist_folder_session.hpp"
#include "workspace/undo_location_session.hpp"

#include "bench/engine_launcher.hpp"
#include "bench/local_library_panel.hpp"
#include "bench/local_list_edit_bar.hpp"
#include "bench/metadata_artwork_section.hpp"
#include "bench/playback_tab_widget.hpp"
#include "bench/quick_pick_popup.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/track_list_find_bar.hpp"
#include "bench/trackknife_style.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "workspace/sources.hpp"
#include <QRandomGenerator>
#include <QStyle>

#include "bench/bench_main_window_helpers.hpp"
#include "uicommon/command_palette.hpp"
#include "uicommon/list_persistence_service.hpp"
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
#include <QCheckBox>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
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
#include <QSysInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
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

// ADR-0292: how wide the sources were left, in pixels.
constexpr auto sources_width_key = "workspace/sources-width";

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
    track_area_->addWidget(tabs_);
    buildListsPanel();

    sources_panel_ = new QWidget(this);
    sources_panel_->setObjectName(QStringLiteral("bench-panel-sources"));
    sources_panel_->setMinimumWidth(160);
    auto* sources_layout = new QVBoxLayout(sources_panel_);
    sources_layout->setContentsMargins(0, 0, 0, 0);
    sources_layout->setSpacing(0);
    auto* heading_row = new QHBoxLayout;
    heading_row->setContentsMargins(6, 6, 6, 4);
    heading_row->setSpacing(2);
    // One-click source switching (ADR-0130). The same tabs as the lists',
    // without close buttons: a source is chosen, not closed.
    local_source_tabs_ = new PlaybackTabBar(sources_panel_, false);
    local_source_tabs_->setDrawBase(false);
    local_source_tabs_->setExpanding(false);
    local_source_tabs_->setUsesScrollButtons(true);
    local_source_tabs_->setElideMode(Qt::ElideRight);
    local_source_tabs_->setObjectName(QStringLiteral("bench-local-source-tabs"));
    local_source_tabs_->setAccessibleName(QStringLiteral("Music source"));
    // ADR-0286: the library, named by the engine holding it -- this
    // computer's here, an engine elsewhere's added when it connects -- and,
    // last, its playlists (ADR-0296).
    local_source_tabs_->addTab(QStringLiteral("Library"));
    local_source_tabs_->setTabToolTip(0, QStringLiteral("The library on this computer"));
    playlists_tab_ = local_source_tabs_->addTab(QStringLiteral("Playlists"));
    local_source_tabs_->setTabToolTip(
        playlists_tab_, QStringLiteral("The library's playlists: the files in its engine's "
                                       "playlist folder, Settings › Engines"));
    heading_row->addWidget(local_source_tabs_, 1);
    // Remembered only when chosen: tabs appearing, hiding or being switched
    // by the window itself do not change what the user asked for.
    connect(local_source_tabs_, &QTabBar::tabBarClicked, this, [this](const int index) {
        const auto kind = local_source_tabs_->tabData(index).toString();
        rememberSource(index == playlists_tab_ ? QStringLiteral("playlists")
                       : !kind.isEmpty()       ? QStringLiteral("remote")
                                               : QStringLiteral("library"));
    });
    // Before the window reacts to changes: the rest of it is not built yet.
    selectPreferredSource();
    connect(local_source_tabs_, &QTabBar::currentChanged, this,
            [this](int) { refreshActiveContext(); });
    sources_layout->addLayout(heading_row);
    source_stack_ = new QStackedWidget(sources_panel_);
    source_stack_->setObjectName(QStringLiteral("bench-source-stack"));
    // No library to show: the engine holding it not reached yet.
    library_missing_ = new QLabel(
        QStringLiteral("The library's engine is not connected. Settings › Engines says which "
                       "holds it."),
        source_stack_);
    library_missing_->setObjectName(QStringLiteral("bench-library-missing"));
    library_missing_->setWordWrap(true);
    library_missing_->setAlignment(Qt::AlignCenter);
    library_missing_->setMargin(12);
    library_missing_->setForegroundRole(QPalette::PlaceholderText);
    source_stack_->addWidget(library_missing_);
    // ADR-0296: the library's playlists -- laid out as the library is: a
    // field above, the rows, and a quiet line below with what can be done.
    playlists_panel_ = new QWidget(source_stack_);
    playlists_panel_->setObjectName(QStringLiteral("bench-playlists-panel"));
    auto* playlists_layout = new QVBoxLayout(playlists_panel_);
    playlists_layout->setContentsMargins(4, 4, 4, 4);
    playlists_layout->setSpacing(4);
    auto* playlists_filter = new QLineEdit(playlists_panel_);
    playlists_filter->setObjectName(QStringLiteral("bench-playlists-filter"));
    playlists_filter->setPlaceholderText(QStringLiteral("Filter playlists"));
    playlists_filter->setClearButtonEnabled(true);
    playlists_layout->addWidget(playlists_filter);
    playlists_view_ = new ListsPanel(playlists_panel_);
    playlists_view_->setObjectName(QStringLiteral("bench-library-playlists"));
    connect(playlists_filter, &QLineEdit::textChanged, playlists_view_, &ListsPanel::setFilter);
    playlists_view_->setDropHandler(
        [this](QDropEvent* drop, const EngineKey& engine, const QString& id) {
            return dropOnPanelList(drop, engine, id);
        });
    connect(playlists_view_, &ListsPanel::listChosen, this,
            [this](const EngineKey& engine, const QString& id) { openEngineList(engine, id); });
    connect(playlists_view_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { showListsMenu(playlists_view_, position); });
    playlists_layout->addWidget(playlists_view_, 1);
    auto* playlists_footer = new RuledFrame(playlists_panel_);
    playlists_footer->setObjectName(QStringLiteral("bench-playlists-footer"));
    auto* footer_row = new QHBoxLayout(playlists_footer);
    footer_row->setContentsMargins(4, 6, 4, 2);
    footer_row->setSpacing(0);
    playlists_status_ = new QLabel(playlists_footer);
    playlists_status_->setObjectName(QStringLiteral("bench-playlists-status"));
    playlists_status_->setWordWrap(true);
    {
        auto small = playlists_status_->font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        playlists_status_->setFont(small);
    }
    playlists_status_->setForegroundRole(QPalette::PlaceholderText);
    footer_row->addWidget(playlists_status_, 1);
    const auto footer_button = [playlists_footer, footer_row](const char* name, QStringView icon,
                                                              const QString& tip) {
        auto* button = new QToolButton(playlists_footer);
        button->setObjectName(QString::fromLatin1(name));
        button->setIcon(themedIcon(icon));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setAutoRaise(true);
        footer_row->addWidget(button, 0, Qt::AlignTop);
        return button;
    };
    connect(footer_button("bench-playlists-new", u"list-add|sp:SP_FileDialogNewFolder",
                          QStringLiteral("New playlist")),
            &QToolButton::clicked, this, &BenchMainWindow::createList);
    connect(footer_button("bench-playlists-folder", u"folder|sp:SP_DirIcon",
                          QStringLiteral("Where the playlists are: Settings › Engines")),
            &QToolButton::clicked, this,
            [this] { static_cast<void>(showSettingsDialog(SettingsDialog::Page::engine)); });
    playlists_layout->addWidget(playlists_footer);
    source_stack_->addWidget(playlists_panel_);
    sources_layout->addWidget(source_stack_, 1);

    // ADR-0292: one layout -- the sources left, the lists right -- and the
    // divider where it was left. The sources keep their width however the
    // window is sized, and the lists take the rest: a window the window
    // manager sizes after it opens finds them as they were.
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName(QStringLiteral("bench-panel-split"));
    split->setChildrenCollapsible(false);
    split->addWidget(sources_panel_);
    split->addWidget(track_area_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);
    sources_split_ = split;
    split->installEventFilter(this);
    connect(split, &QSplitter::splitterMoved, this, [split](const int, const int) {
        QSettings{}.setValue(QLatin1String(sources_width_key), split->sizes().value(0));
    });

    selection_status_ = new QLabel(statusBar());
    selection_status_->setObjectName(QStringLiteral("bench-selection-status"));
    selection_status_->setAccessibleName(QStringLiteral("Selected track information"));
    selection_status_->setContentsMargins(6, 0, 6, 0);
    selection_status_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusBar()->addWidget(selection_status_, 1);
    refreshSelectionStatus();

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
    quit->setStatusTip(QStringLiteral("Close Melody and stop this computer's engine"));
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
    identify_action_ = edit_menu->addAction(QStringLiteral("Identify…"));
    identify_action_->setObjectName(QStringLiteral("action-identify"));
    identify_action_->setToolTip(
        QStringLiteral("Look up the selected tracks' albums on MusicBrainz -- the whole list "
                       "when none are selected -- in the tag editor"));
    identify_action_->setEnabled(false);
    connect(identify_action_, &QAction::triggered, this, &BenchMainWindow::showIdentify);
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
    track_layout_menu_ = workspace_menu->addMenu(QStringLiteral("Track list layout"));
    auto* track_layout_menu = track_layout_menu_;
    track_layout_menu->setObjectName(QStringLiteral("menu-track-list-layout"));
    track_presentation_group_ = new QActionGroup(track_layout_menu);
    track_presentation_group_->setExclusive(true);
    // ADR-0282: every list view, the shipped and the user's own, filled in
    // before this.
    list_views_end_ = track_layout_menu->addSeparator();
    auto* edit_view = track_layout_menu->addAction(QStringLiteral("Edit list view…"));
    edit_view->setObjectName(QStringLiteral("action-edit-list-view-menu"));
    connect(edit_view, &QAction::triggered, this, [this] { editListView(false); });
    auto* new_view = track_layout_menu->addAction(QStringLiteral("New list view…"));
    new_view->setObjectName(QStringLiteral("action-new-list-view-menu"));
    connect(new_view, &QAction::triggered, this, [this] { editListView(true); });
    connect(&ListViewCatalog::instance(), &ListViewCatalog::changed, this,
            &BenchMainWindow::redrawListViews);
    connect(&ListViewCatalog::instance(), &ListViewCatalog::changed, this,
            &BenchMainWindow::rebuildListViewActions);
    rebuildListViewActions();
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
    refreshTabActions();
    refreshTrackViewActions();
}

void BenchMainWindow::placeSources() {
    // The width kept, or a quarter of the window when none is, as much of
    // it as leaves the lists room; not kept anew, so a window made narrow
    // and wide again finds the sources as wide as they were.
    const auto extent = sources_split_->width();
    const auto kept = QSettings{}.value(QLatin1String(sources_width_key), 0).toInt();
    const auto room = std::max(1, extent - track_area_->minimumSizeHint().width());
    const auto sources = std::min(kept > 0 ? kept : extent / 4, room);
    sources_split_->setSizes({sources, std::max(1, extent - sources)});
}

} // namespace trackknife::bench

void trackknife::bench::BenchMainWindow::openQuickPick(const QuickPickKind kind) {
    // ADR-0286: the library, on the engine holding it, whichever tab is in
    // front -- as search asks it.
    const auto engine = workspace_.libraryKey();
    auto* library = libraryOf(engine);
    const auto* source = catalogueOf(engine);
    if (library == nullptr || source == nullptr) {
        showMessage(QStringLiteral("The library's engine is not connected"), 4'000);
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
    // ADR-0286: every engine's music folders, a page each, naming the engine
    // it is of ("local", or its address) and what is known of it: the
    // Engines page shows the one holding the library. The same folders its
    // library's Folders… opens.
    std::function<QWidget*(QWidget*)> library_folders;
    if (std::ranges::any_of(engines_,
                            [](const auto& engine) { return engine->library != nullptr; })) {
        library_folders = [this](QWidget* parent) -> QWidget* {
            auto* pages = new QWidget(parent);
            for (const auto& engine : engines_) {
                if (engine->library == nullptr) {
                    continue;
                }
                auto* folders = engine->library->createFoldersWidget(pages);
                folders->setProperty("engine-address",
                                     engine->key.isLocal()
                                         ? QString::fromLatin1(SettingsKeys::library_engine_local)
                                         : engine->setting.address);
                folders->setProperty("engine-state",
                                     engine->catalogue != nullptr && engine->catalogue->reachable()
                                         ? tr("connected")
                                         : tr("not reached"));
            }
            return pages;
        };
    }
    // ADR-0296: where each engine's playlists are, a page each as its music
    // folders are: the Engines page shows the one holding the library.
    std::function<QWidget*(QWidget*)> playlist_folders;
    if (std::ranges::any_of(engines_,
                            [](const auto& engine) { return engine->file_work != nullptr; })) {
        playlist_folders = [this](QWidget* parent) -> QWidget* {
            auto* pages = new QWidget(parent);
            for (const auto& engine : engines_) {
                if (engine->file_work == nullptr) {
                    continue;
                }
                const auto name =
                    engine->key.isLocal() ? tr("This computer") : engineName(engine->key);
                auto* session = new PlaylistFolderSession(
                    engine->file_work,
                    engine->key.isLocal() ? EngineFolderLister{}
                                          : workspace_.engineFolderLister(engine->file_work),
                    pages);
                auto* shown = new PlaylistFolderWidget(*session, name, pages);
                session->setParent(shown);
                shown->setProperty("engine-address",
                                   engine->key.isLocal()
                                       ? QString::fromLatin1(SettingsKeys::library_engine_local)
                                       : engine->setting.address);
            }
            return pages;
        };
    }
    // ADR-0266: where each engine keeps its undo copies, asked of it.
    std::function<QWidget*(QWidget*)> undo_locations;
    if (std::ranges::any_of(engines_,
                            [](const auto& engine) { return engine->file_work != nullptr; })) {
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
    // ADR-0322: whether each engine watches its library's folders, asked of
    // it; the Engines page shows the one chosen there.
    std::function<QWidget*(QWidget*)> library_watch = [this](QWidget* parent) -> QWidget* {
        auto* pages = new QWidget(parent);
        for (const auto& engine : engines_) {
            if (engine->catalogue == nullptr || !engine->catalogue->endpoint()) {
                continue;
            }
            auto* row = new QWidget(pages);
            row->setProperty("engine-address",
                              engine->key.isLocal()
                                  ? QString::fromLatin1(SettingsKeys::library_engine_local)
                                  : engine->setting.address);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* box = new QCheckBox(tr("Watch its library folders for changes"), row);
            box->setObjectName(QStringLiteral("bench-settings-library-watch-box"));
            box->setToolTip(
                tr("Files added, changed, moved or deleted in its music folders are taken in at "
                   "once, without Refresh. Changes made on another machine -- a NAS writing to "
                   "its own disk under a share this engine mounts -- are not seen; melody-watch, "
                   "run there, tells of those. Changing this restarts the engine."));
            auto* note = new QLabel(row);
            note->setForegroundRole(QPalette::PlaceholderText);
            layout->addWidget(box);
            layout->addWidget(note, 1);
            auto* session = new LibraryWatchSession(*engine->catalogue->endpoint(), row);
            const auto show = [box, note, session] {
                const QSignalBlocker blocker{box};
                box->setChecked(session->watching());
                box->setEnabled(session->known() && !session->busy());
                note->setText(session->status());
            };
            connect(session, &LibraryWatchSession::changed, row, show);
            connect(box, &QCheckBox::toggled, session, &LibraryWatchSession::setWatching);
            show();
            session->load();
        }
        return pages;
    };
    auto* dialog = new SettingsDialog(
        this, buildOutputProfileStore(), std::move(library_folders),
        [this](QWidget* parent) { return buildLastFmSettings(parent); }, configurable_shortcuts_,
        std::move(undo_locations), std::move(playlist_folders), std::move(library_watch));
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
        // ADR-0291: a plugin enabled or turned off brings or takes its panels.
        buildPluginPanels();
        buildPluginTriggers();
        applyLocalLibraryVisibility();
        for (auto* library : findChildren<LocalLibraryPanel*>()) {
            library->applyAppearance();
        }
        // ADR-0286: which engine holds the library decides how tabs are
        // marked and ordered.
        for (const auto& tab : list_tabs_) {
            refreshTabChrome(*tab);
            refreshEmptyMessage(*tab);
        }
        keepTabGroupsTogether();
        // The playlists shown are the library's, wherever it now is.
        followLibraryPlaylists();
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
