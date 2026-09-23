// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/engine_playback.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/local_list_model.hpp"
#include "bench/local_playback_service.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "bench/settings_dialog.hpp"
#include "trackknife/audio/album_grouping.hpp"
#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "trackknife/audio/request_queue.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/listen_accounting.hpp"
#include "trackknife/core/local_sources.hpp"
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

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class QActionGroup;
class QDialog;
class QDockWidget;
class QLabel;
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

namespace trackknife::audio {
class LocalAuditionService;
struct LocalAuditionSnapshot;
} // namespace trackknife::audio

namespace trackknife::ui {
class QueueTableView;
class ListPersistenceService;
class LocalFolderTreeModel;
} // namespace trackknife::ui

namespace trackknife::query {
struct CompiledTkq;
}

namespace trackknife::bench {
struct ConvertDialogItem;

struct MetadataOperationJobOutcome;
class MusicBrainzFetchService;
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
// Widget properties, persisted UI state and JSON all carry a document identity
// as text. ADR-0220 Phase 0 makes the playback anchor hold the identity itself,
// so these two are the only places the two spellings meet.
[[nodiscard]] inline QString document_text(const core::StableId& id) {
    return id.is_nil() ? QString{} : QString::fromStdString(id.to_string());
}

[[nodiscard]] inline core::StableId document_identity(const QString& text) {
    auto parsed = core::StableId::parse(text.toStdString());
    return parsed ? *parsed : core::StableId{};
}

class BenchMainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit BenchMainWindow(QWidget* parent = nullptr);
    ~BenchMainWindow() override;

    BenchMainWindow(const BenchMainWindow&) = delete;
    BenchMainWindow& operator=(const BenchMainWindow&) = delete;

    void importM3u8Path(std::string raw_path);
    void openLocalPaths(std::vector<std::string> raw_paths);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    friend class BenchMainWindowTest;
    bool handleTabTrackDrop(QAbstractItemView* source, QDropEvent* event, const QPoint& position);
    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
        QByteArray preserved_view_layout;
        bool view_layout_persistence_protected{false};
    };

    void buildPlaylistActions(QMenu* file_menu);
    void importPlaylistDialog();
    void exportPlaylistDialog();
    void buildWorkspace();
    void buildTransport();
    void buildUpNext();
    void refreshUpNext();
    void enqueueUpNext(QTableView* source, bool prepend, int position = -1);
    void enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position = -1);
    void addUpNextActions(QMenu* menu, QTableView* source);
    void editUpNext(int operation, int row = -1, int destination = -1);
    void editUpNextSelection(int operation, int destination = -1);
    void persistUpNext();
    void restoreUpNext();
    void restoreLocalResume();
    void checkpointLocalResume(const audio::LocalAuditionSnapshot& snapshot, bool force = false);
    [[nodiscard]] bool playLocalRequest(std::optional<std::int64_t> restore_position_ms = {});
    void adoptLocalRequest(audio::RequestQueue<LocalTrackRow>::Entry entry, bool restoring = false);

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
    void restoreLists(std::vector<persistence::ListDocument> documents);
    void schedulePersist();
    void persistNow(bool wait);
    void backupWorkspace();
    void scheduleWorkspaceRestore();
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();
    [[nodiscard]] std::vector<persistence::TrackViewPreset> collectTrackViewLayouts();
    void refreshActiveContext();
    void addLocalRateMenus(QTableView* view, ListTab* source_tab);
    void addLocalRateMenus(QMenu* menu, QTableView* view);
    void refreshLocalRatings();

    void showDynamicPlaylists();

    ListTab* addListTab(persistence::ListDocument document, bool select);
    [[nodiscard]] ListTab* currentListTab();
    // ADR-0153: the standalone search dialog, created lazily, one instance.
    void openSearchDialog();
    // ADR-0156: shared capture/apply plumbing for Properties and the
    // compact context-menu ReplayGain dialog.
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows);
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(LocalListModel* model, std::vector<QPersistentModelIndex> rows);
    [[nodiscard]] MetadataWritePlanApplierFactory metadataPlanApplierFactory();
    [[nodiscard]] MetadataApplyObserver metadataApplyObserver();
    void showReplayGainDialog();
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id);
    // ADR-0220 Phase 0: playback state names its document by identity, not
    // by a rendered QString. Widget properties still carry the text form, so
    // both spellings resolve to the same tab.
    [[nodiscard]] ListTab* tabForDocument(const core::StableId& document_id);
    bool transferRows(QTableView* source, const QVariantList& rows, const QString& target_id,
                      bool move, int insertion_row);
    bool transferRowsToNewTab(QTableView* source, const QVariantList& rows, bool move,
                              const QString& name);
    struct CrossTabMoveEdit {
        QString source_id;
        QString target_id;
        std::vector<LocalTrackRow> source_before;
        std::vector<LocalTrackRow> source_after;
        std::vector<LocalTrackRow> target_before;
        std::vector<LocalTrackRow> target_after;
        bool applied{true};
    };
    [[nodiscard]] bool canReplayCrossTabMove(bool undo);
    bool replayCrossTabMove(bool undo);
    void refreshTabChrome(ListTab& tab);
    void setActiveLocalList(const QString& id);
    void refreshPlaybackCursor(bool jump = false);
    void buildShortcuts();
    void showCommandPalette();
    QList<QAction*> configurable_shortcuts_;
    QAction* follow_playback_action_{};
    QPointer<QTableView> followed_playback_view_;
    QPersistentModelIndex followed_playback_index_;
    void refreshTabActions();
    void refreshListHistoryActions();
    void replayListEdit(bool undo);
    void markTabDirty(ListTab& tab);
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
    void persistFolderBookmarks() const;
    void addFolderBookmark(const std::string& raw_path);
    void revealFolderPath(const std::string& raw_path);
    void revealFolderStep(const QPersistentModelIndex& parent_index, const std::string& raw_path);
    void playCurrentRow();
    void showMetadataProperties();
    void openMetadataProperties(std::size_t count, MetadataPropertiesSourceReader reader);
    void showConvertDialog();
    void showConvertForView(QTableView* view);
    void openConvertItems(std::vector<ConvertDialogItem> items);
    void showMetadataForView(QTableView* view);
    void showReplayGainForView(QTableView* view);
    SettingsDialog* showSettingsDialog(SettingsDialog::Page page = SettingsDialog::Page::general);
    [[nodiscard]] OutputProfileStore buildOutputProfileStore();
    void startMetadataOperationRecovery();
    [[nodiscard]] MusicBrainzLookupService musicBrainzLookupService();
    void finishMetadataOperationJob();
    void presentInterruptedOperations();
    void applyCommittedMetadata(const operations::MetadataCommitResult& result);
    void applyCommittedCueReplayGain(const operations::CueReplayGainCommitResult& result);
    void applyCommittedLoudnessSidecar(const operations::LoudnessSidecarCommitResult& result);
    void applyCommittedRelocation(const operations::FilePublicationCommitResult& result);
    void applyCommittedPublicationMetadata(const operations::FilePublicationCommitResult& result,
                                           const metadata::MetadataDocument& document);
    void removeSelectedRows();
    void transferSelectedRows(QTableView* source, const QString& target_id, bool move);
    [[nodiscard]] ui::TrackViewLayout
    defaultTrackViewLayout(ui::TrackViewPresentation presentation =
                               ui::TrackViewPresentation::albums_side_artwork) const;
    void applyTrackViewLayout(ListTab& tab, const ui::TrackViewLayout& layout);
    [[nodiscard]] ui::TrackViewLayout captureTrackViewLayout(const ListTab& tab) const;
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
    void refreshSelectionStatus();
    void refreshSelectionActions();
    [[nodiscard]] QTableView* activeTrackView();
    void showTrackViewHeaderMenu(QTableView* view, const QPoint& position);

    void openFilesDialog();
    void openFolderDialog();
    void addFolderRoot();
    void startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                        int insertion_row, bool replace_and_play = false);
    void finishDiscovery();

    struct DiscoveryOutcome {
        std::vector<LocalTrackRow> rows;
        std::vector<core::LocalSourceIssue> issues;
        bool cancelled{false};
        bool truncated{false};
    };

    struct ProbeJob {
        QString document_id;
        std::string raw_path;
        int hint_row{-1};
    };
    struct ProbeOutcome {
        ProbeJob job;
        std::vector<LocalTrackRow> rows;
        LocalTrackRow whole_file_fallback;
    };
    void enqueueUnprobedRows(ListTab& tab);
    void pumpProbeQueue();
    void finishProbeBatch();

    struct ArtworkJob {
        QString key;
        std::string raw_path;
    };
    struct ArtworkOutcome {
        QString key;
        QImage image;
    };
    void syncArtwork(ListTab& tab);
    void invalidateArtwork(const std::string& raw_path);
    void pumpArtworkQueue();
    void finishArtworkLoad();

    void buildLocalPlaybackControls(QMenu* playback_menu);
    void refreshLocalPlaybackControls();
    void saveLocalPlaybackModes();
    void applyLocalPlaybackModes();
    void showReplayGainPreampDialog();
    void resetPlaybackOrder();
    void prepareAlbumPlaybackOrder(std::uint64_t generation);
    void adoptPlaybackRow(ListTab& tab, int row, const LocalTrackSource& source, bool consume,
                          int direction = 1);
    // ADR-0221: the entry to consume is named by identity, with the row it
    // last occupied as a lookup hint.
    void consumePlaybackRow(ListTab& tab, const core::StableId& entry, int hint_row);
    // Resolve the playing entry to its current row in `tab`, or -1 when the
    // entry is no longer there. playback_row_ serves as the lookup hint.
    [[nodiscard]] int resolvePlaybackRow(const ListTab* tab) const;
    // Adopts whatever the engine is already playing. An engine outlives the
    // window, so a window that only learns about playback by having started it
    // shows nothing after a restart while the music is still going.
    void reattachToEngine();
    // Mirrors the up-next panel onto the engine, which is what makes Next
    // play a requested track rather than the next row of the list. Cheap when
    // nothing changed.
    void syncEngineRequests();
    // Keeps the engine's queue and the playing list in step, in both
    // directions: an edit here is pushed, and a change the engine made that
    // this window did not cause is read back.
    void syncEngineQueue();
    void adoptEngineQueue();
    // Credits listening to Last.fm from the engine's state rather than from a
    // local player that is not running.
    void sampleLastFmFromEngine(const EnginePlayback::State& state);
    // Points the workspace at an entry the engine is playing.
    void adoptEngineRow(ListTab& tab, int row, const core::StableId& entry);
    // True when playback belongs to an engine rather than to this process.
    [[nodiscard]] bool playingOnEngine() const;
    // The transport view when the engine owns playback: the workspace's own
    // up-next, resume, listening and gapless are the engine's job then, so
    // this is only the controls and the cursor.
    void refreshEngineTransport();
    [[nodiscard]] std::optional<std::pair<int, LocalTrackSource>> automaticPlaybackRow();
    void playRow(ListTab& tab, int row, std::optional<std::int64_t> restore_position_ms = {});
    void playAdjacent(int direction);
    [[nodiscard]] std::optional<std::pair<int, LocalTrackSource>>
    adjacentPlaybackRow(int direction);
    void refreshTransport();
    void buildMprisService();
    void publishMprisState();
    void rebuildDeviceMenu();
    void configurePlaybackBuffer(const QString& profile, int capacity_ms, int start_threshold_ms);
    void showCustomPlaybackBufferDialog();
    void refreshPlaybackBufferChecks();
    void reloadPlaybackPreferences();
    void togglePlayPause();
    void seekToMs(qint64 position_ms);

    audio::LocalAuditionService* player_{nullptr};
    std::unique_ptr<audio::LocalAuditionService> player_storage_;

    ui::LocalFolderTreeModel* folder_model_{nullptr};
    LocalLibraryPanel* local_library_{nullptr};
    // Folders and Library, plus -- while the tag editor is open -- a
    // temporary page hosting its file list (ADR-0183 addendum).
    QTabBar* local_source_tabs_{nullptr};
    QTreeView* folder_view_{nullptr};
    QTabWidget* tabs_{nullptr};
    std::vector<std::unique_ptr<ListTab>> list_tabs_;
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
    QHash<QString, QByteArray> restored_track_view_layouts_;

    MprisService* mpris_{nullptr};
    DesktopNotifier* notifier_{nullptr};
    QAction* notifications_action_{nullptr};
    ui::ListPersistenceService* persistence_{nullptr};
    std::filesystem::path database_path_;
    // ADR-0220: one place decides whether a catalogue is this process or an
    // engine, and everything that needs one asks here.
    std::unique_ptr<CatalogueSource> catalogue_source_;
    // ADR-0220: when an engine is configured it owns playback, and the
    // workspace drives it instead of its own player. Null when there is no
    // engine, which is the unchanged local path.
    EnginePlayback* engine_playback_{nullptr};
    // The entry the engine last reported. The engine advances its own queue,
    // so without following it the highlighted row would stay on whatever was
    // double-clicked while something else played.
    QString engine_entry_;
    // The request order last stated to the engine, so an unchanged panel does
    // not re-send it on every refresh.
    QString engine_requests_;
    // The last entry the engine reported consuming, so one drop is mirrored
    // once however often the state is sampled.
    QString engine_consumed_;
    // The queue last pushed to the engine, and the engine revision that
    // produced. Together they answer "is what I am showing what the engine
    // holds, and if not, who changed it".
    QString engine_queue_;
    quint64 engine_queue_revision_{0};
    MusicBrainzFetchService* musicbrainz_service_{nullptr};
    QTimer* persistence_timer_{nullptr};
    QTimer* transport_timer_{nullptr};

    QFutureWatcher<DiscoveryOutcome> discovery_watcher_;
    QString discovery_target_document_;
    int discovery_insertion_row_{-1};
    QPersistentModelIndex discovery_insertion_anchor_;
    bool discovery_anchored_{false};
    bool discovery_replace_and_play_{false};
    bool discovery_running_{false};

    QFutureWatcher<std::vector<ProbeOutcome>> probe_watcher_;
    std::deque<ProbeJob> probe_queue_;
    bool probe_running_{false};
    core::CancellationSource probe_cancellation_;

    QFutureWatcher<void> artwork_watcher_;
    std::shared_ptr<ArtworkOutcome> artwork_outcome_;
    std::deque<ArtworkJob> artwork_queue_;
    QHash<QString, QImage> artwork_cache_;
    QSet<QString> artwork_pending_;
    QSet<QString> artwork_invalidated_while_loading_;
    bool artwork_running_{false};
    std::optional<CrossTabMoveEdit> cross_tab_move_edit_;

    QFutureWatcher<std::shared_ptr<MetadataOperationJobOutcome>> metadata_operation_watcher_;
    std::shared_ptr<MetadataOperationJobOutcome> metadata_operation_snapshot_;
    core::CancellationSource metadata_operation_cancellation_;
    QPointer<QDialog> interrupted_operations_dialog_;
    bool metadata_operation_running_{false};
    bool metadata_recovery_started_{false};

    // Paths opened before the asynchronous list restore finishes are queued
    // and flushed into the initial tab once it exists.
    std::vector<std::string> pending_open_paths_;
    bool lists_restored_{false};
    bool resume_restore_pending_{false};
    bool resume_save_pending_{false};
    std::uint64_t resume_intent_generation_{0};
    QElapsedTimer resume_save_clock_;

    QAction* local_repeat_action_{nullptr};
    QAction* local_random_action_{nullptr};
    QAction* local_album_random_action_{nullptr};
    QAction* local_single_action_{nullptr};
    QAction* local_consume_action_{nullptr};
    std::vector<QToolButton*> local_mode_buttons_;
    QToolButton* local_replaygain_button_{nullptr};
    QActionGroup* local_replaygain_group_{nullptr};
    bool album_order_preparing_{false};
    std::uint64_t album_order_generation_{0};
    int album_order_build_row_{0};
    // ADR-0220 Phase 0: the grouping rule and its limits are Qt-free policy;
    // the chunked walk and the timers driving it stay here.
    audio::AlbumGrouper album_grouper_;
    QString local_replaygain_{QStringLiteral("off")};
    double local_rg_preamp_with_{0.0};
    double local_rg_preamp_without_{0.0};
    std::optional<ListTab> detached_playback_;
    std::optional<audio::RequestQueue<LocalTrackRow>::Entry> requested_request_;
    std::optional<audio::RequestQueue<LocalTrackRow>::Entry> queued_request_;
    QDockWidget* up_next_dock_{nullptr};
    QToolButton* up_next_button_{nullptr};
    ui::QueueTableView* up_next_view_{nullptr};
    LocalListModel* up_next_local_model_{nullptr};
    QLabel* up_next_status_{nullptr};
    std::vector<std::uint64_t> up_next_display_ids_;
    bool up_next_restored_{false};
    std::uint64_t up_next_local_revision_{0};

    // ADR-0220 Phase 0: the playback service. The mode actions, transport
    // buttons and up-next view above are its views.
    LocalPlaybackService playback_;
    bool consuming_row_{false};
    LastFmService* lastfm_{};
    QElapsedTimer lastfm_clock_;
    qint64 lastfm_sample_time_{-1000};
    void buildLastFm();
    void sampleLastFm(const audio::LocalAuditionSnapshot& snapshot);
    core::ListenAccounting local_listen_accounting_;
    QElapsedTimer local_history_clock_;
    void sampleListeningHistory(const audio::LocalAuditionSnapshot& snapshot, qint64 monotonic_ms,
                                qint64 wall_ms);
    QWidget* buildLastFmSettings(QWidget* parent);
    void addLastFmActions(QMenu* menu, QTableView* view);
    // Last explicitly played local list; transport stop does not release it.
    QString active_local_list_id_;
    bool advance_pending_{false};
    // Gapless continuation upkeep: the last takeover count seen, the last
    // requested next path, and a throttle for re-requests after the engine
    // dropped or rejected a queue.
    quint64 last_chain_transitions_{0U};
    std::uint64_t last_requested_token_{0U};
    QElapsedTimer next_request_timer_;
    bool seeking_{false};
    QToolButton* mute_button_{nullptr};
    QHash<QString, int> unmuted_volumes_;
    void refreshMuteButton();
    bool changing_volume_{false};
    QString last_player_error_;
    QString last_device_monitor_error_;
    QString last_output_recovery_error_;
    QString selected_buffer_profile_{QStringLiteral("balanced")};
    std::vector<std::pair<std::string, std::string>> device_choices_;
    std::optional<std::string> selected_device_;
    std::optional<std::string> default_device_;
    bool selected_device_available_{true};
    quint64 last_device_generation_{0U};
};

} // namespace trackknife::bench
