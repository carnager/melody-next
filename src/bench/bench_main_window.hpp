// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "workspace/folder_browser.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_view.hpp"
#include "bench/engine_key.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/local_list_model.hpp"
#include "bench/local_playback_service.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/remote_engines.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "trackknife/audio/request_queue.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/operations/cue_replay_gain_apply.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/loudness_sidecar_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/panel_layout.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

class QActionGroup;
class QDialog;
class QDockWidget;
class QDropEvent;
class QLabel;
class QPushButton;
class QStyledItemDelegate;
class QListWidget;
class QLineEdit;
class QMenu;
class QPoint;
class QResizeEvent;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QAbstractItemView;
class QTableView;
class QTreeWidget;
class QTimer;
class QToolButton;
class QTreeView;
class QVBoxLayout;

namespace trackknife::audio {} // namespace trackknife::audio

namespace trackknife::ui {
class QueueTableView;
class ListPersistenceService;
class LocalFolderTreeModel;
} // namespace trackknife::ui

namespace trackknife::query {
struct CompiledTkq;
}

namespace trackknife::ui {
class LocalFilesMimeData;
}

namespace trackknife::bench {
enum class QuickPickKind;
class ListsPanel;
struct ConvertDialogItem;

class LocalLibraryPanel;
class MetadataPropertiesDialog;
class SearchDialog;
class DesktopNotifier;
class MprisService;
class TrackListFindBar;
class LocalListEditBar;
class PlaylistTransferBar;

// Trackknife main window: composed Folders/Track Lists panels, configurable
// local working-list views, and one transport over the serialized playback worker
// (ADR-0021/0023/0024, promoted to first-class playback by ADR-0025).
class BenchMainWindow final : public QMainWindow, public WorkspaceView {
    Q_OBJECT
    // ADR-0220: the workspace's state and behaviour live in Workspace, which
    // both windows are drawn over; this window reaches them through these
    // names while its logic moves across.
    using ListTab = Workspace::ListTab;
    using CrossTabMoveEdit = Workspace::CrossTabMoveEdit;
    using PendingRelocation = Workspace::PendingRelocation;
    using DiscoveryOutcome = Workspace::DiscoveryOutcome;
    using ProbeJob = Workspace::ProbeJob;
    using ProbeOutcome = Workspace::ProbeOutcome;
    using ArtworkJob = Workspace::ArtworkJob;
    using ArtworkOutcome = Workspace::ArtworkOutcome;
    using SeenEngine = Workspace::SeenEngine;
    using EngineLink = Workspace::EngineLink;
    using EngineInterruption = Workspace::EngineInterruption;
    using ModeText = Workspace::ModeText;
    // In the window's object tree, so what it holds is found through it.
    Workspace workspace_{this};
    std::vector<std::unique_ptr<ListTab>>& list_tabs_{workspace_.list_tabs_};
    QHash<QString, QByteArray>& restored_track_view_layouts_{workspace_.restored_track_view_layouts_};
    MprisService*& mpris_{workspace_.mpris_};
    DesktopNotifier*& notifier_{workspace_.notifier_};
    ui::ListPersistenceService*& persistence_{workspace_.persistence_};
    std::filesystem::path& database_path_{workspace_.database_path_};
    std::vector<std::unique_ptr<EngineLink>>& engines_{workspace_.engines_};
    EngineListSync*& list_sync_{workspace_.list_sync_};
    std::vector<PendingRelocation>& pending_relocations_{workspace_.pending_relocations_};
    EnginePlayback*& transport_{workspace_.transport_};
    EngineKey& up_next_engine_{workspace_.up_next_engine_};
    QString& engine_entry_{workspace_.engine_entry_};
    QString& engine_requests_{workspace_.engine_requests_};
    QString& engine_consumed_{workspace_.engine_consumed_};
    QString& engine_queue_{workspace_.engine_queue_};
    quint64& engine_queue_revision_{workspace_.engine_queue_revision_};
    quint64& engine_queue_asked_{workspace_.engine_queue_asked_};
    quint64& engine_reattach_asked_{workspace_.engine_reattach_asked_};
    QTimer*& persistence_timer_{workspace_.persistence_timer_};
    QFutureWatcher<DiscoveryOutcome>& discovery_watcher_{workspace_.discovery_watcher_};
    QString& discovery_target_document_{workspace_.discovery_target_document_};
    int& discovery_insertion_row_{workspace_.discovery_insertion_row_};
    QPersistentModelIndex& discovery_insertion_anchor_{workspace_.discovery_insertion_anchor_};
    bool& discovery_anchored_{workspace_.discovery_anchored_};
    bool& discovery_replace_and_play_{workspace_.discovery_replace_and_play_};
    bool& discovery_running_{workspace_.discovery_running_};
    QFutureWatcher<std::vector<ProbeOutcome>>& probe_watcher_{workspace_.probe_watcher_};
    std::deque<ProbeJob>& probe_queue_{workspace_.probe_queue_};
    bool& probe_running_{workspace_.probe_running_};
    core::CancellationSource& probe_cancellation_{workspace_.probe_cancellation_};
    QFutureWatcher<void>& artwork_watcher_{workspace_.artwork_watcher_};
    std::shared_ptr<ArtworkOutcome>& artwork_outcome_{workspace_.artwork_outcome_};
    std::deque<ArtworkJob>& artwork_queue_{workspace_.artwork_queue_};
    QHash<QString, QImage>& artwork_cache_{workspace_.artwork_cache_};
    QSet<QString>& artwork_pending_{workspace_.artwork_pending_};
    QSet<QString>& artwork_invalidated_while_loading_{workspace_.artwork_invalidated_while_loading_};
    bool& artwork_running_{workspace_.artwork_running_};
    std::optional<CrossTabMoveEdit>& cross_tab_move_edit_{workspace_.cross_tab_move_edit_};
    std::vector<EngineInterruption>& engine_interruptions_{workspace_.engine_interruptions_};
    std::vector<persistence::SavedDestinationProfile>& local_destinations_{workspace_.local_destinations_};
    QThreadPool& layout_pushes_{workspace_.layout_pushes_};
    std::vector<std::string>& pending_open_paths_{workspace_.pending_open_paths_};
    bool& lists_restored_{workspace_.lists_restored_};
    QString& local_replaygain_{workspace_.local_replaygain_};
    double& local_rg_preamp_with_{workspace_.local_rg_preamp_with_};
    double& local_rg_preamp_without_{workspace_.local_rg_preamp_without_};
    std::optional<ListTab>& detached_playback_{workspace_.detached_playback_};
    LocalListModel*& up_next_local_model_{workspace_.up_next_local_model_};
    std::vector<std::uint64_t>& up_next_display_ids_{workspace_.up_next_display_ids_};
    bool& up_next_restored_{workspace_.up_next_restored_};
    std::uint64_t& up_next_local_revision_{workspace_.up_next_local_revision_};
    LocalPlaybackService& playback_{workspace_.playback_};
    bool& consuming_row_{workspace_.consuming_row_};
    LastFmService*& lastfm_{workspace_.lastfm_};
    QElapsedTimer& lastfm_clock_{workspace_.lastfm_clock_};
    qint64& lastfm_sample_time_{workspace_.lastfm_sample_time_};
    QString& lastfm_user_{workspace_.lastfm_user_};
    QString& active_local_list_id_{workspace_.active_local_list_id_};

  public:
    explicit BenchMainWindow(QWidget* parent = nullptr);
    ~BenchMainWindow() override;

    BenchMainWindow(const BenchMainWindow&) = delete;
    BenchMainWindow& operator=(const BenchMainWindow&) = delete;

    void importM3u8Path(std::string raw_path);
    void openLocalPaths(std::vector<std::string> raw_paths) {
        workspace_.openLocalPaths(std::move(raw_paths));
    }

    // WorkspaceView
    void showMessage(const QString& text, int timeout_ms) override;
    void listAdded(ListTab& tab, bool select) override;
    void showList(ListTab& tab) override;
    [[nodiscard]] ListTab* currentList() override;
    [[nodiscard]] std::vector<ListTab*> listsInOrder() override;
    [[nodiscard]] QObject* modelParent() override;
    [[nodiscard]] int currentRow(const ListTab& tab) override;
    void artworkLoaded(const QString& key) override;
    void workspaceRestored(bool restored) override;
    void engineConnected(EngineLink& engine, bool first) override;
    void engineAttached(EngineLink& engine) override;
    void engineRekeyed(EngineLink& engine, const EngineKey& from, bool lists_follow) override;
    void engineRemoving(EngineLink& engine) override;
    void engineRemoved() override;
    void enginesSynced() override;
    void engineListsChanged() override { fetchEngineLists(); }
    void engineRatingsChanged(const EngineKey& engine,
                              const QHash<QString, unsigned>& ratings) override;
    void engineInterruptionsChanged(bool reported_now) override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    friend class BenchMainWindowTest;
    // Files dropped on the tab bar: into the tab under them, or into a new
    // one when dropped on empty space.
    bool handleTabFileDrop(QDropEvent* drop, int tab_index);
    bool handleTabTrackDrop(QAbstractItemView* source, QDropEvent* event, const QPoint& position);
    bool handleTrackDropOnTab(QAbstractItemView* source, QDropEvent* drop, int tab_index);

    void buildPlaylistActions(QMenu* file_menu);
    void importPlaylistDialog();
    void exportPlaylistDialog();
    void buildWorkspace();
    void buildTransport();
    void buildUpNext();
    void refreshUpNext() override;
    // Queues what was dragged out of a library, with its tags.
    bool enqueueLibraryDrop(const ui::LocalFilesMimeData& files, int position);
    void enqueueUpNext(QTableView* source, bool prepend, int position = -1);
    // `engine`: whose files these are (ADR-0227). Up Next holds one engine's
    // asks at a time.
    void enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position = -1,
                              const EngineKey& engine = EngineKey::local()) {
        workspace_.enqueueLocalRequests(std::move(rows), position, engine);
    }
    void addUpNextActions(QMenu* menu, QTableView* source);
    void editUpNext(int operation, int row = -1, int destination = -1) {
        workspace_.editUpNext(operation, row, destination);
    }
    void editUpNextSelection(int operation, int destination = -1);
    void persistUpNext() { workspace_.persistUpNext(); }
    void restoreUpNext() { workspace_.restoreUpNext(); }

    [[nodiscard]] ui::PanelLayout defaultPanelLayout() const;
    void loadPanelLayout();
    void applyPanelLayout(const ui::PanelLayout& layout);
    [[nodiscard]] QWidget* renderPanelLayoutNode(const ui::PanelLayoutNode& node, QWidget* parent);
    [[nodiscard]] ui::PanelLayoutNode capturePanelLayoutNode(QWidget* widget) const;
    void persistPanelLayout();
    void setLayoutEditMode(bool editing);
    void arrangePanelLayout(ui::PanelLayoutNodeKind kind, Qt::Orientation orientation);
    void swapPanelLayout();
    void resetPanelLayout();
    void refreshPanelLayoutActions();
    void initializePersistence();
    void schedulePersist() { workspace_.schedulePersist(); }
    void persistNow(bool wait) { workspace_.persistNow(wait); }
    // ADR-0233: another client's version of a list open here, and a save of
    // one that someone else saved first.
    void settleListConflict(const QString& id);
    // The lists of every engine this window reaches, to open one as a tab.
    void showOpenListDialog();
    // ADR-0233: this computer's tabs before the remote engine's, always.
    void keepTabGroupsTogether();
    // Where a view's engine stands among this window's: 0 for this computer.
    [[nodiscard]] int engineRank(const QWidget* view) const;
    bool regrouping_tabs_{false};
    // The open lists of one engine, in tab order, by id and name.
    [[nodiscard]] std::vector<std::pair<QString, QString>> listTargets(const EngineKey& engine) const {
        return workspace_.listTargets(engine);
    }
    // Then, whatever is to be done with it once it is open.
    void openEngineList(const EngineKey& key, const QString& id, std::function<void()> then = {}) {
        workspace_.openEngineList(key, id, std::move(then));
    }
    // ADR-0233: the lists as a pane beside the tracks instead of as a tab
    // bar, by the setting below.
    static constexpr auto lists_display_key = "appearance/lists-display";
    void buildListsPanel();
    void applyListsDisplay();
    [[nodiscard]] bool listsInPanel() const;
    // Asks the engines for their lists again, a moment later.
    void fetchEngineLists();
    // Shows the window's tabs and the engines' lists in the pane, soon.
    void refreshListsPanel();
    void presentListsPanel();
    void showListsPanelMenu(const QPoint& position);
    bool dropOnPanelList(QDropEvent* drop, const EngineKey& engine, const QString& id);
    void addDroppedPaths(const QString& id, const EngineKey& files_engine,
                         std::vector<std::string> paths);
    QSplitter* track_area_{nullptr};
    QWidget* lists_pane_{nullptr};
    ListsPanel* lists_panel_{nullptr};
    QAction* lists_panel_action_{nullptr};
    QTimer* lists_fetch_timer_{nullptr};
    QTimer* lists_present_timer_{nullptr};
    void backupWorkspace();
    void scheduleWorkspaceRestore();
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments() {
        return workspace_.collectDocuments();
    }
    void refreshActiveContext();
    void addLocalRateMenus(QTableView* view, ListTab* source_tab);
    void addLocalRateMenus(QMenu* menu, QTableView* view);
    void refreshLocalRatings() { workspace_.refreshRatings(); }
    // A rating an engine says was set -- here, on the phone, from a script:
    // shown in the tabs whose tracks are that engine's.
    void adoptEngineRating(const EngineKey& engine, const QString& hash, unsigned rating) {
        workspace_.adoptEngineRating(engine, hash, rating);
    }

    void showDynamicPlaylists();

    ListTab* addListTab(persistence::ListDocument document, bool select) {
        return workspace_.addList(std::move(document), select);
    }
    [[nodiscard]] ListTab* currentListTab();
    // ADR-0153: the standalone search dialog, created lazily, one instance.
    void openSearchDialog();
    // ADR-0156: shared capture/apply plumbing for Properties and the
    // compact context-menu ReplayGain dialog.
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows);
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(LocalListModel* model, std::vector<QPersistentModelIndex> rows,
                          std::optional<std::vector<LocalTrackRow>> snapshot = std::nullopt);
    [[nodiscard]] MetadataApplyObserver metadataApplyObserver();
    void showReplayGainDialog();
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id) {
        return workspace_.tabForDocument(document_id);
    }
    // ADR-0220 Phase 0: playback state names its document by identity, not
    // by a rendered QString. Widget properties still carry the text form, so
    // both spellings resolve to the same tab.
    [[nodiscard]] ListTab* tabForDocument(const core::StableId& document_id) {
        return workspace_.tabForDocument(document_id);
    }
    bool transferRows(QTableView* source, const QVariantList& rows, const QString& target_id,
                      bool move, int insertion_row);
    bool transferRowsToNewTab(QTableView* source, const QVariantList& rows, bool move,
                              const QString& name);
    [[nodiscard]] bool canReplayCrossTabMove(bool undo) { return workspace_.canReplayCrossTabMove(undo); }
    bool replayCrossTabMove(bool undo) { return workspace_.replayCrossTabMove(undo); }
    void refreshTabChrome(ListTab& tab) override;
    void setActiveLocalList(const QString& id) { workspace_.setActiveLocalList(id); }
    void refreshPlaybackCursor(bool jump = false) override;
    void buildShortcuts();
    void showCommandPalette();
    // Ctrl+L: the search field of the library showing -- or, from Folders,
    // of the library the source switch would open.
    void focusLibrarySearch();
    // Ctrl+Shift+A / Ctrl+Shift+T: find an album or a track by words and
    // add, replace or queue it.
    void openQuickPick(QuickPickKind kind);
    QList<QAction*> configurable_shortcuts_;
    QAction* follow_playback_action_{};
    QPointer<QTableView> followed_playback_view_;
    QPersistentModelIndex followed_playback_index_;
    void refreshTabActions();
    void refreshListHistoryActions() override;
    void replayListEdit(bool undo);
    // An edit made here: saved, and -- to the list playing -- told to its
    // engine, whose queue it is.
    void markTabDirty(ListTab& tab) { workspace_.markTabDirty(tab); }
    // A change the engine made -- its queue adopted, a consumed row dropped:
    // saved, never sent back. Sent back, it is this window's copy of the
    // engine's queue replacing the queue itself, tags another client gave it
    // and all.
    void takeEngineChange(ListTab& tab) { workspace_.takeEngineChange(tab); }
    void closeTabAt(int index);
    // Most-recently-visited tabs, newest first, so closing one returns to
    // where you came from rather than to its neighbour.
    QList<QPointer<QWidget>> tab_visit_history_;
    void rememberTabVisit(QWidget* tab);
    [[nodiscard]] QPointer<QWidget> previouslyVisitedTab(QWidget* closed) const;
    void closeCurrentTab();
    void createList();
    void duplicateCurrentTab();
    void toggleCurrentTabPinned();
    void saveCurrentList();
    void renameCurrentList();
    void showTabContextMenu(const QPoint& position);
    void showTrackContextMenu(QTableView* view, const QPoint& position);
    void showFolderContextMenu(const QPoint& position);
    void showFolderBookmarkMenu(const QPoint& position);
    void loadFolderBookmarks();
    void addFolderBookmark(const std::string& raw_path) { folder_browser_->addBookmark(raw_path); }
    void revealFolderPath(const std::string& raw_path) { folder_browser_->reveal(raw_path); }
    void playCurrentRow();
    void showMetadataProperties();
    // ADR-0237: `work` is the engine that does the file work, or null for
    // this process.
    void openMetadataProperties(std::size_t count, MetadataPropertiesSourceReader reader,
                                std::shared_ptr<engine::RemoteFileWork> work);
    void showConvertDialog();
    void showConvertForView(QTableView* view);
    void openConvertItems(std::vector<ConvertDialogItem> items);
    void showMetadataForView(QTableView* view);
    void showReplayGainForView(QTableView* view);
    SettingsDialog* showSettingsDialog(SettingsDialog::Page page = SettingsDialog::Page::general);
    // Naming layouts, and the move destinations of `destinations_of`; with
    // every engine's destinations as places for the manager (ADR-0237).
    [[nodiscard]] OutputProfileStore
    buildOutputProfileStore(const EngineKey& destinations_of = EngineKey::local());
    // ADR-0237: the naming layouts are global; every engine that does file
    // work holds a copy, handed over after each change and when it connects.
    // Only `removed` is taken away there: nothing an engine holds is lost to
    // a set it was sent.
    void pushLayouts(std::vector<core::StableId> removed = {}) {
        workspace_.pushLayouts(std::move(removed));
    }
    void presentInterruptedOperations();
    void applyCommittedMetadata(const operations::MetadataCommitResult& result);
    void applyCommittedCueReplayGain(const operations::CueReplayGainCommitResult& result);
    void applyCommittedLoudnessSidecar(const operations::LoudnessSidecarCommitResult& result);
    void applyCommittedRelocation(const operations::FilePublicationCommitResult& result);
    void queueEngineRelocation(const std::string& from, const std::string& to) {
        workspace_.queueEngineRelocation(from, to);
    }
    void flushEngineRelocations() { workspace_.flushEngineRelocations(); }
    void applyCommittedPublicationMetadata(const operations::FilePublicationCommitResult& result,
                                           const metadata::MetadataDocument& document);
    void removeSelectedRows();
    void transferSelectedRows(QTableView* source, const QString& target_id, bool move);
    [[nodiscard]] static ui::TrackViewLayout
    defaultTrackViewLayout(ui::TrackViewPresentation presentation =
                               ui::TrackViewPresentation::albums_side_artwork) {
        return Workspace::defaultTrackViewLayout(presentation);
    }
    void applyTrackViewLayout(ListTab& tab, const ui::TrackViewLayout& layout);
    [[nodiscard]] ui::TrackViewLayout captureTrackViewLayout(const ListTab& tab) const override;
    void applyTrackViewLayout(QTableView* view, ui::TrackViewLayout& state,
                              const ui::TrackViewLayout& layout);
    [[nodiscard]] ui::TrackViewLayout
    captureTrackViewLayout(const QTableView* view, const ui::TrackViewLayout& state) const;
    void setTrackViewPresentation(ui::TrackViewPresentation presentation);
    void setTrackColumnVisible(const QString& column_id, bool visible);
    void resetTrackViewLayout();
    void copyTrackViewLayoutToAllTabs();
    void refreshTrackViewActions();
    void stopBackgroundWork();
    void refreshSelectionStatus() override;
    void refreshSelectionActions();
    [[nodiscard]] QTableView* activeTrackView();
    void showTrackViewHeaderMenu(QTableView* view, const QPoint& position);

    void openFilesDialog();
    void openFolderDialog();
    void addFolderRoot();
    void startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                        int insertion_row, bool replace_and_play = false) {
        workspace_.startDiscovery(std::move(raw_paths), std::move(target_document_id), insertion_row,
                                  replace_and_play);
    }


    void enqueueUnprobedRows(ListTab& tab) { workspace_.enqueueUnprobedRows(tab); }
    // ADR-0226: this computer's engine outlives the window, so a rebuilt or
    // updated one keeps running the old program until it is restarted --
    // done here, at once when nothing plays, else when playback stops.
    void renewOutdatedLocalEngine() { workspace_.renewOutdatedLocalEngine(); }
    // Shows or hides this computer's library tab, as Settings says.
    void applyLocalLibraryVisibility();
    // What an empty list tab says, and how to fill it.
    [[nodiscard]] QString emptyListTitle(const EngineKey& engine) const {
        return workspace_.emptyListTitle(engine);
    }
    [[nodiscard]] QString emptyListHint(const EngineKey& engine) const {
        return workspace_.emptyListHint(engine);
    }
    // How an engine is named to the user: "this computer", or its name.
    [[nodiscard]] QString engineName(const EngineKey& engine) const { return workspace_.engineName(engine); }
    // Shows the source the user last chose, or a library by default.
    void selectPreferredSource();
    [[nodiscard]] bool localLibraryShown() const;
    bool& engine_renewal_pending_{workspace_.engine_renewal_pending_};
    // Quit, as opposed to closing the window: this computer's engine stops
    // too, instead of playing on.
    void quitAndStopEngine();
    // ADR-0227, ADR-0234: paths moving from one engine's list to another's,
    // as the other engine sees them (RemoteMount). What cannot be -- not
    // reachable here, or not in the remote's library -- is left out, and the
    // status bar says how much and why.
    [[nodiscard]] std::optional<std::string> crossEnginePath(const std::string& path, const EngineKey& from,
                                                           const EngineKey& to) const {
        return workspace_.crossEnginePath(path, from, to);
    }
    [[nodiscard]] std::vector<std::string> crossEnginePaths(std::vector<std::string> paths,
                                                          const EngineKey& from, const EngineKey& to) {
        return workspace_.crossEnginePaths(std::move(paths), from, to);
    }
    // An engine's library folders, asked of it; empty when it is not
    // reachable, and then nothing is known to cross to it.
    // Where an engine's music is reachable here, as Settings say now: a change
    // there applies at once, as the one remote's always did.
    [[nodiscard]] RemoteMount mountOf(const EngineLink& engine) const { return workspace_.mountOf(engine); }
    // Rows from paths a remote engine gave -- a drag from its library --
    // without looking for them on this computer, where they need not be.
    void insertRemotePaths(ListTab& tab, std::vector<std::string> raw_paths, int insertion_row) {
        workspace_.insertRemotePaths(tab, std::move(raw_paths), insertion_row);
    }

    void syncArtwork(ListTab& tab) { workspace_.syncArtwork(tab); }
    void invalidateArtwork(const std::string& raw_path) { workspace_.invalidateArtwork(raw_path); }

    void buildLocalPlaybackControls(QMenu* playback_menu);
    void refreshLocalPlaybackControls() override;
    void saveLocalPlaybackModes() { workspace_.saveLocalPlaybackModes(); }
    void styleStatusBar();
    void applyLocalPlaybackModes() { workspace_.applyLocalPlaybackModes(); }
    void showReplayGainPreampDialog();
    // Resolve the playing entry to its current row in `tab`, or -1 when the
    // entry is no longer there. playback_row_ serves as the lookup hint.
    [[nodiscard]] int resolvePlaybackRow(const ListTab* tab) const { return workspace_.resolvePlaybackRow(tab); }
    // Adopts whatever the engine is already playing. An engine outlives the
    // window, so a window that only learns about playback by having started it
    // shows nothing after a restart while the music is still going.
    void reattachToEngine() { workspace_.reattachToEngine(); }
    // Mirrors the up-next panel onto the engine, which is what makes Next
    // play a requested track rather than the next row of the list. Cheap when
    // nothing changed.
    void syncEngineRequests() { workspace_.syncEngineRequests(); }
    // Keeps the engine's queue and the playing list in step, in both
    // directions: an edit here is pushed, and a change the engine made that
    // this window did not cause is read back.
    void syncEngineQueue() { workspace_.syncEngineQueue(); }
    void adoptEngineQueue() { workspace_.adoptEngineQueue(); }
    // Credits listening to Last.fm from the engine's state rather than from a
    // local player that is not running.
    void sampleLastFmFromEngine(const EnginePlayback::State& state) {
        workspace_.sampleLastFm(state);
    }
    // Points the workspace at an entry the engine is playing.
    // Makes `playback` the one the transport follows, stopping the other if it
    // was playing: one engine plays at a time.
    // `stop_other`: the engine followed until now is stopped -- as when this
    // window starts playing elsewhere. Not when another client did it: then
    // the window only looks where the music started.
    void followPlayback(EnginePlayback* playback, bool stop_other = true) {
        workspace_.followPlayback(playback, stop_other);
    }
    // Another client -- a script, the phone, a picker -- started the engine
    // this window does not follow: the window follows the music there.
    void followIfStartedElsewhere(EnginePlayback* playback) { workspace_.followIfStartedElsewhere(playback); }
    // What each engine was last seen doing, so a start is told from a state
    // the window already knew of.
    void rememberEngineState(EnginePlayback* playback) { workspace_.rememberEngineState(playback); }
    // Builds the remote connection, its library panel and its default tab.
    // ADR-0234: one engine elsewhere, as Settings name it; the first is the
    // one an older release's remote lists belong to.
    void connectRemoteEngine(const RemoteEngineSetting& setting, bool first) {
        workspace_.connectRemoteEngine(setting, first);
    }
    // Settings changed: engines listed and not connected are connected,
    // those connected and no longer listed -- or now at another address or
    // with another password -- are let go, and connected again if listed.
    void syncRemoteEngines() { workspace_.syncRemoteEngines(); }
    [[nodiscard]] ListTab* remoteQueueTab() { return workspace_.remoteQueueTab(); }
    // An engine's own tab: its first, or one made for it, named after it.
    [[nodiscard]] ListTab* engineTab(EngineLink& engine) { return workspace_.engineTab(engine); }
    // True while an engine is connected. ADR-0226: nothing plays otherwise;
    // this window has no player of its own.
    [[nodiscard]] bool playingOnEngine() const { return workspace_.playingOnEngine(); }
    // The transport view: controls, cursor, output and buffer, all from the
    // engine's state. Resume, listening and gapless are the engine's own.
    void refreshEngineTransport();
    void refreshOutputControls(const EnginePlayback::State& state);
    // The row for the entry the engine is playing, wherever this window holds
    // it: the list it was played from, Up Next, or another open list.
    [[nodiscard]] const LocalTrackRow* playingRow(const QString& entry) { return workspace_.playingRow(entry); }
    // The cover of the playing entry's album, from a tab that has it.
    void refreshHeaderCover(const QString& entry);
    // An album's cover from the lists or the cache; fetched when neither has
    // it, arriving later through the same path as a tab's.
    [[nodiscard]] QImage coverFor(const LocalTrackRow& track, const EngineKey& engine) {
        return workspace_.coverFor(track, engine);
    }
    void setUpNextCount(int count);
    void playRow(ListTab& tab, int row) { workspace_.playRow(tab, row); }
    void refreshTransport() override;
    void buildMprisService();
    void publishMprisState() { workspace_.publishDesktopState(); }
    void rebuildDeviceMenu();
    // A mode's icon marked as one-shot: on for one track, then off.
    [[nodiscard]] QIcon oneShotIcon(const QIcon& plain) const;
    void configurePlaybackBuffer(const QString& profile, int capacity_ms, int start_threshold_ms) {
        workspace_.configurePlaybackBuffer(profile, capacity_ms, start_threshold_ms);
    }
    void showCustomPlaybackBufferDialog();
    void refreshPlaybackBufferChecks() override;
    void reloadPlaybackPreferences();
    void togglePlayPause() { workspace_.togglePlayPause(); }
    void seekToMs(qint64 position_ms) { workspace_.seekToMs(position_ms); }

    FolderBrowser* folder_browser_{nullptr};
    ui::LocalFolderTreeModel* folder_model_{nullptr};
    // Folders and Library, plus -- while the tag editor is open -- a
    // temporary page hosting its file list (ADR-0183 addendum).
    QTabBar* local_source_tabs_{nullptr};
    QTreeView* folder_view_{nullptr};
    QTabWidget* tabs_{nullptr};
    QPointer<SearchDialog> search_dialog_;
    QAction* replaygain_action_{nullptr};

    QAction* previous_action_{nullptr};
    QAction* play_pause_action_{nullptr};
    // QAction::setIcon cannot compare icons, so it fires changed on every
    // call — and each ActionChanged makes QToolButton::setDefaultAction add
    // another connection. The 30 Hz transport refresh must therefore only
    // touch the icon when the playing state actually flips.
    std::optional<bool> transport_icon_playing_;
    QAction* stop_action_{nullptr};
    QAction* next_action_{nullptr};
    QAction* duplicate_tab_action_{nullptr};
    QAction* pin_tab_action_{nullptr};
    QAction* save_tab_action_{nullptr};
    QAction* rename_tab_action_{nullptr};
    QAction* close_tab_action_{nullptr};
    QAction* play_selected_action_{nullptr};
    QAction* properties_action_{nullptr};
    QAction* convert_action_{nullptr};
    QAction* remove_selected_action_{nullptr};
    QAction* undo_list_action_{nullptr};
    QAction* redo_list_action_{nullptr};
    TrackListFindBar* list_find_bar_{nullptr};
    PlaylistTransferBar* playlist_transfer_bar_{nullptr};
    QAction* export_playlist_action_{nullptr};
    LocalListEditBar* list_edit_bar_{nullptr};
    QMenu* sort_list_menu_{nullptr};
    QAction* reverse_list_action_{nullptr};
    QAction* shuffle_albums_action_{nullptr};
    QAction* deduplicate_list_action_{nullptr};
    QAction* find_list_action_{nullptr};
    QAction* find_next_action_{nullptr};
    QAction* find_previous_action_{nullptr};
    QAction* folder_add_to_list_action_{nullptr};
    QAction* folder_toggle_expanded_action_{nullptr};
    QAction* layout_edit_action_{nullptr};
    QAction* layout_side_by_side_action_{nullptr};
    QAction* layout_top_bottom_action_{nullptr};
    QAction* layout_tabbed_action_{nullptr};
    QAction* layout_swap_action_{nullptr};
    QAction* layout_reset_action_{nullptr};
    QAction* track_albums_side_action_{nullptr};
    QAction* track_albums_header_action_{nullptr};
    QAction* track_plain_columns_action_{nullptr};
    QAction* track_compact_queue_action_{nullptr};
    QAction* track_layout_reset_action_{nullptr};
    QAction* track_layout_copy_action_{nullptr};
    QSlider* seek_{nullptr};
    QLabel* elapsed_{nullptr};
    QLabel* duration_{nullptr};
    QLabel* now_playing_{nullptr};
    QLabel* now_playing_context_{nullptr};
    QLabel* now_playing_cover_{nullptr};
    // The group whose cover the header shows, so a tick does not rescale it.
    QString header_cover_key_;
    // The album whose cover the header is waiting for, and the entry playing.
    QString header_cover_wanted_;
    QString header_cover_entry_;
    QLabel* selection_status_{nullptr};
    QSlider* volume_{nullptr};
    QToolButton* device_button_{nullptr};
    QMenu* device_menu_{nullptr};
    QActionGroup* device_group_{nullptr};
    QMenu* buffer_menu_{nullptr};
    QActionGroup* buffer_group_{nullptr};
    QMenu* tab_context_menu_{nullptr};
    QMenu* track_context_menu_{nullptr};
    QMenu* folder_context_menu_{nullptr};
    QActionGroup* layout_arrangement_group_{nullptr};
    QActionGroup* track_presentation_group_{nullptr};
    QMenu* track_columns_menu_{nullptr};
    QHash<QString, QAction*> track_column_actions_;

    QWidget* layout_host_{nullptr};
    QVBoxLayout* layout_host_layout_{nullptr};
    QWidget* layout_root_{nullptr};
    QWidget* folders_panel_{nullptr};
    QListWidget* folder_bookmarks_{nullptr};
    QLabel* folder_bookmarks_heading_{nullptr};
    QMenu* folder_bookmark_menu_{nullptr};
    QAction* folder_bookmark_add_action_{nullptr};
    QAction* folder_bookmark_remove_action_{nullptr};
    QStackedWidget* source_stack_{nullptr};
    QHash<QString, QWidget*> panel_widgets_;
    bool applying_panel_layout_{false};
    bool panel_layout_persistence_protected_{false};
    bool applying_track_view_layout_{false};

    QAction* notifications_action_{nullptr};
    [[nodiscard]] EngineLink* link(const EngineKey& key) const { return workspace_.link(key); }
    [[nodiscard]] EngineLink& localEngine() const { return *engines_.front(); }
    // The remote configured in Settings -- by its id once it has said it,
    // "remote" until then. Null when none is configured.
    [[nodiscard]] EngineLink* remoteEngine() const {
        for (const auto& engine : engines_) {
            if (!engine->key.isLocal()) {
                return engine.get();
            }
        }
        return nullptr;
    }
    // The link a connection belongs to; null for none of this window's.
    [[nodiscard]] EngineLink* linkOf(const EnginePlayback* playback) const { return workspace_.linkOf(playback); }
    // The engine that does the file work for a view's files, if one does.
    [[nodiscard]] std::shared_ptr<engine::RemoteFileWork> fileWorkOf(QTableView* view) const;
    // That engine, or -- having said in the status bar why `what` cannot be
    // done -- nothing.
    [[nodiscard]] std::shared_ptr<engine::RemoteFileWork> requireFileWork(QTableView* view,
                                                                          const QString& what);
    [[nodiscard]] MetadataWritePlanApplierFactory
    engineMetadataPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work);
    [[nodiscard]] ArtworkWritePlanApplierFactory
    engineArtworkPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work);
    // Stage 5: moves and renames the engine makes. For an engine elsewhere,
    // `mounted` receives each move as this computer sees it through the
    // engine's mount -- this computer's paths and revisions -- for its own
    // lists to follow.
    using MountedMoves = std::vector<operations::FilePublicationCommitResult>;
    [[nodiscard]] FilePublicationPlanApplierFactory
    enginePublicationPlanApplierFactory(std::shared_ptr<engine::RemoteFileWork> work,
                                        bool elsewhere, RemoteMount mount,
                                        std::shared_ptr<MountedMoves> mounted);
    // An engine elsewhere moved a file: its own tabs follow at its paths,
    // everything of this computer's at `here`, the move seen through the
    // mount, when it is.
    void applyEngineRelocation(const EngineKey& engine,
                               const operations::FilePublicationCommitResult& result,
                               const operations::FilePublicationCommitResult* here);
    // The engine link whose file-work connection this is.
    [[nodiscard]] const EngineLink* linkOfWork(const engine::RemoteFileWork* work) const;
    // Parts of an engine's link; null when it or the part is not there.
    [[nodiscard]] EnginePlayback* playbackOf(const EngineKey& key) const {
        return workspace_.playbackOf(key);
    }
    [[nodiscard]] CatalogueSource* catalogueOf(const EngineKey& key) const {
        return workspace_.catalogueOf(key);
    }
    [[nodiscard]] LocalLibraryPanel* libraryOf(const EngineKey& key) const;
    [[nodiscard]] EnginePlayback* localPlayback() const { return localEngine().playback; }
    [[nodiscard]] EnginePlayback* remotePlayback() const {
        return remoteEngine() != nullptr ? remoteEngine()->playback : nullptr;
    }
    [[nodiscard]] CatalogueSource* localCatalogue() const { return localEngine().catalogue.get(); }
    [[nodiscard]] CatalogueSource* remoteCatalogue() const {
        return remoteEngine() != nullptr ? remoteEngine()->catalogue.get() : nullptr;
    }
    [[nodiscard]] LocalLibraryPanel* localLibrary() const { return localEngine().library; }
    [[nodiscard]] LocalLibraryPanel* remoteLibrary() const {
        return remoteEngine() != nullptr ? remoteEngine()->library : nullptr;
    }
    QTimer* transport_timer_{nullptr};




    QPointer<QDialog> interrupted_operations_dialog_;


    QAction* local_repeat_action_{nullptr};
    QAction* local_random_action_{nullptr};
    QAction* local_album_random_action_{nullptr};
    QAction* local_single_action_{nullptr};
    QAction* local_consume_action_{nullptr};
    std::vector<QToolButton*> local_mode_buttons_;
    QToolButton* local_replaygain_button_{nullptr};
    QActionGroup* local_replaygain_group_{nullptr};
    QDockWidget* up_next_dock_{nullptr};
    QToolButton* up_next_button_{nullptr};
    QLabel* up_next_badge_{nullptr};
    QLabel* device_chevron_{nullptr};
    ui::QueueTableView* up_next_view_{nullptr};
    QLabel* up_next_status_{nullptr};

    void buildLastFm() { workspace_.startLastFm(); }
    QWidget* buildLastFmSettings(QWidget* parent);
    // ADR-0220: an engine's own Last.fm state, into `state`; and this
    // window's session handed to it, so it scrobbles what it plays.
    void askEngineLastFm(const protocol::Endpoint& endpoint, QLabel* state, QPushButton* use);
    void handOverLastFm(const protocol::Endpoint& endpoint, QLabel* state, QPushButton* use);
    // "In use" when the engine already scrobbles as this window's account.
    void showEngineAccount(QPushButton* use, const QString& engine_user);
    void addLastFmActions(QMenu* menu, QTableView* view);
    bool seeking_{false};
    QToolButton* mute_button_{nullptr};
    QHash<QString, int> unmuted_volumes_;
    void refreshMuteButton();
    bool changing_volume_{false};
    QString& selected_buffer_profile_{workspace_.selected_buffer_profile_};
};

} // namespace trackknife::bench
