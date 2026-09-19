// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "bench/settings_dialog.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "bench/musicbrainz_identify_dialog.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "trackknife/core/cancellation.hpp"
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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QActionGroup;
class QDialog;
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
class MelodyAgentService;
} // namespace trackknife::audio

namespace trackknife::ui {
class ListPersistenceService;
class LocalFolderTreeModel;
class ServerLibraryTreeModel;
class ServerLibraryTreeView;
} // namespace trackknife::ui

namespace trackknife::mpd {
struct Track;
} // namespace trackknife::mpd

namespace trackknife::quick {
class MpdProbeController;
class MpdQueueModel;
class MpdSearchResultModel;
} // namespace trackknife::quick

namespace trackknife::bench {

struct MetadataOperationJobOutcome;
class MusicBrainzFetchService;
class LocalLibraryPanel;
class MetadataPropertiesDialog;
class SearchDialog;
class MpdLibrarySearchModel;
class DesktopNotifier;
class MprisService;
class TrackListFindBar;
class LocalListEditBar;
class PlaylistTransferBar;

// Trackknife main window: composed Folders/Track Lists panels, configurable
// local working-list views, and one transport over the serialized playback worker
// (ADR-0021/0023/0024, promoted to first-class playback by ADR-0025).
class BenchMainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit BenchMainWindow(QWidget* parent = nullptr);
    ~BenchMainWindow() override;

    BenchMainWindow(const BenchMainWindow&) = delete;
    BenchMainWindow& operator=(const BenchMainWindow&) = delete;

    void importM3u8Path(std::string raw_path);
    void openLocalPaths(std::vector<std::string> raw_paths);
    void loadMpdUrisAsLocalFiles(const QStringList& uris);
    // ADR-0180: file-operation sugar on mapped MPD selections — materialize
    // through the load-as-local-files bridge, then open the named dialog on
    // the created tab once its asynchronous discovery finishes.
    enum class MaterializedDialog : std::uint8_t { none, edit_tags, replay_gain, convert };
    void materializeMpdSelectionForDialog(const QStringList& uris, MaterializedDialog dialog);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    friend class BenchMainWindowTest;
    bool handleTabTrackDrop(QTableView* source, QDropEvent* event, const QPoint& position);
    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
        QByteArray preserved_view_layout;
        bool view_layout_persistence_protected{false};
    };

    // Server-authoritative stored-playlist tab (ADR-0129): keyed by the MPD
    // playlist name, session-only, refreshed exclusively from server re-reads.
    struct MpdPlaylistTab {
        QString name;
        quick::MpdQueueModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
    };

    // Committed search-result tab (ADR-0140): keyed by the search query,
    // session-only, holding the finished search's track-hit snapshot.
    // Recommitting the same query refreshes the tab in place.
    struct MpdSearchTab {
        QString query;
        quick::MpdQueueModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
    };

    void buildPlaylistActions(QMenu* file_menu);
    void importPlaylistDialog();
    void exportPlaylistDialog();
    void buildWorkspace();
    void buildMpdWorkspace();
    void buildMpdSearch();
    void buildMpdStatusControls();
    void buildTransport();
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
    void showMpdDiagnostics();
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();
    [[nodiscard]] std::vector<persistence::TrackViewPreset> collectTrackViewLayouts();
    void openMpdConnectionDialog();
    void autoConnectMpd();
    void refreshActiveContext();
    [[nodiscard]] bool isMpdContext() const;
    void previewMpdSearch();
    void finishMpdSearch(const QString& query, bool success);
    void updateMpdSearchPresentation();
    void activateMpdSearchResult(const QModelIndex& index, int action, int insertion_row = -1);
    void refreshMpdStatusControls();
    void activateMpdLibraryAction(const QModelIndex& index, int action);
    void completePendingMpdLibraryAction();
    void showMpdLibraryContextMenu(const QPoint& position);
    [[nodiscard]] QVariantList selectedMpdQueueRows() const;
    [[nodiscard]] QStringList selectedMpdQueueUris() const;
    void refreshMpdPriorityMenu();
    // ADR-0179: shared star-rating submenus for both authorities, and the
    // debounced local rating reload from the content-identity store.
    void refreshMpdRateMenu();
    void addLocalRateMenus(QTableView* view, ListTab* source_tab);
    void refreshLocalRatings();

    void buildMpdPlaylists();
    [[nodiscard]] MpdPlaylistTab* mpdPlaylistTabForWidget(QWidget* widget) const;
    [[nodiscard]] MpdPlaylistTab* currentMpdPlaylistTab() const;
    [[nodiscard]] MpdPlaylistTab* mpdPlaylistTabNamed(const QString& name) const;
    void openMpdPlaylistTab(const QString& name, bool select);
    void acceptMpdStoredPlaylistNames(const QStringList& names);
    void acceptMpdStoredPlaylistContents(const QString& name);
    void renameMpdPlaylistTab(const QString& from, const QString& to);
    void refreshMpdPlaylistContextMarkers();
    void persistOpenPlaylistTabs();
    void restoreOpenPlaylistTabs(const QStringList& available);
    void closeMpdPlaylistTab(const QString& name);
    void refreshMpdPlaylistsSoon();
    void showMpdPlaylistSidebarMenu(const QPoint& position);
    void showMpdPlaylistTrackMenu(MpdPlaylistTab& tab, const QPoint& position);
    [[nodiscard]] MpdSearchTab* mpdSearchTabForWidget(QWidget* widget) const;
    [[nodiscard]] MpdSearchTab* currentMpdSearchTab() const;
    [[nodiscard]] MpdSearchTab* mpdSearchTabForQuery(const QString& query) const;
    void commitMpdSearchTab();
    void openMpdSearchTab(const QString& query, std::vector<mpd::Track> tracks, bool select);
    void closeMpdSearchTab(MpdSearchTab* tab);
    void showMpdSearchTrackMenu(MpdSearchTab& tab, const QPoint& position);
    void addMpdPlaylistActions(QMenu* menu, const QString& name);
    void promptSaveQueueAsPlaylist();
    void promptRenameMpdPlaylist(const QString& name);
    void confirmClearMpdPlaylist(const QString& name);
    void confirmDeleteMpdPlaylist(const QString& name);
    [[nodiscard]] QStringList selectedMpdViewUris(QTableView* view) const;
    void addMappedLocalTrackActions(QMenu* menu, const QStringList& uris,
                                    const QString& object_prefix);

    ListTab* addListTab(persistence::ListDocument document, bool select);
    [[nodiscard]] QString effectiveMpdMusicRoot() const;
    [[nodiscard]] std::optional<core::StableId> currentMpdProfileId() const;
    ListTab* materializeMpdSelectionAsLocalTab(const QStringList& uris);
    void maybeOpenMaterializedDialog();
    [[nodiscard]] ListTab* currentListTab();
    // ADR-0153: the standalone search dialog, created lazily, one instance.
    void openSearchDialog();
    // ADR-0156: shared capture/apply plumbing for Properties and the
    // compact context-menu ReplayGain dialog.
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows);
    [[nodiscard]] MetadataWritePlanApplierFactory metadataPlanApplierFactory();
    [[nodiscard]] MetadataApplyObserver metadataApplyObserver();
    void showReplayGainDialog();
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id);
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
    void refreshTabActions();
    void refreshListHistoryActions();
    void replayListEdit(bool undo);
    void markTabDirty(ListTab& tab);
    void closeTabAt(int index);
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
    void goToMpdLibraryEntry(const QString& artist, const QString& album);
    void completeMpdLibraryGoTo(const QString& artist, const QString& album);
    void playCurrentRow();
    void showMetadataProperties();
    void showConvertDialog();
    void showSettingsDialog(SettingsDialog::Page page = SettingsDialog::Page::general);
    [[nodiscard]] OutputProfileStore buildOutputProfileStore();
    void applyLibraryOrder(bool persist);
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
    void adoptPlaybackRow(ListTab& tab, int row, const LocalTrackSource& source, bool consume,
                          int direction = 1);
    void consumePlaybackRow(ListTab& tab, const QPersistentModelIndex& index);
    [[nodiscard]] std::optional<std::pair<int, LocalTrackSource>> automaticPlaybackRow();
    void playRow(ListTab& tab, int row);
    void playAdjacent(int direction);
    [[nodiscard]] std::optional<std::pair<int, LocalTrackSource>>
    adjacentPlaybackRow(int direction);
    void refreshTransport();
    void refreshMpdTransport();
    void refreshMelodyEndpoint();
    void buildMprisService();
    void publishMprisState();
    void rebuildDeviceMenu();
    void configurePlaybackBuffer(const QString& profile, int capacity_ms, int start_threshold_ms);
    void showCustomPlaybackBufferDialog();
    void refreshPlaybackBufferChecks();
    void togglePlayPause();
    void seekToMs(qint64 position_ms);

    audio::LocalAuditionService* player_{nullptr};
    std::unique_ptr<audio::LocalAuditionService> player_storage_;
    std::unique_ptr<audio::LocalAuditionService> melody_player_storage_;
    std::unique_ptr<audio::MelodyAgentService> melody_endpoint_;
    QString melody_endpoint_profile_;

    ui::LocalFolderTreeModel* folder_model_{nullptr};
    LocalLibraryPanel* local_library_{nullptr};
    QTabBar* local_source_tabs_{nullptr};
    // ADR-0183 addendum: temporary sidebar page hosting the active tag
    // editor's file list.
    QWidget* properties_files_page_{nullptr};
    QTableView* properties_files_view_{nullptr};
    QLabel* properties_files_dir_{nullptr};
    QStyledItemDelegate* properties_files_delegate_{nullptr};
    QPointer<MetadataPropertiesDialog> hosted_properties_;
    int previous_local_source_index_{-1};
    void updatePropertiesFileHosting();
    QTabBar* mpd_source_tabs_{nullptr};
    QStackedWidget* mpd_source_pages_{nullptr};
    QTreeView* folder_view_{nullptr};
    quick::MpdProbeController* mpd_controller_{nullptr};
    ui::ServerLibraryTreeModel* server_library_model_{nullptr};
    ui::ServerLibraryTreeView* server_library_view_{nullptr};
    QTableView* mpd_queue_view_{nullptr};
    QLineEdit* mpd_search_field_{nullptr};
    QWidget* mpd_library_panel_{nullptr};
    QStackedWidget* mpd_library_stack_{nullptr};
    QWidget* mpd_search_surface_{nullptr};
    quick::MpdSearchResultModel* mpd_search_model_{nullptr};
    MpdLibrarySearchModel* mpd_search_tree_model_{nullptr};
    QTreeView* mpd_search_view_{nullptr};
    QLabel* mpd_search_status_{nullptr};
    QTimer* mpd_search_timer_{nullptr};
    ui::TrackViewLayout mpd_view_layout_;
    QByteArray preserved_mpd_view_layout_;
    bool mpd_view_layout_persistence_protected_{false};
    std::vector<persistence::ConnectionProfile> mpd_profiles_;
    bool mpd_was_connected_{false};
    enum class MpdLibraryAction : std::uint8_t {
        append,
        next,
        replace,
        insert,
        load_local,
        update_directory,
        edit_tags,
        replay_gain,
        convert,
    };
    std::optional<MpdLibraryAction> pending_mpd_library_action_;
    QPersistentModelIndex pending_mpd_library_index_;
    int pending_mpd_library_insertion_row_{-1};
    QTabWidget* tabs_{nullptr};
    std::vector<std::unique_ptr<ListTab>> list_tabs_;
    QPointer<SearchDialog> search_dialog_;
    QAction* replaygain_action_{nullptr};
    std::vector<std::unique_ptr<MpdPlaylistTab>> mpd_playlist_tabs_;
    std::vector<std::unique_ptr<MpdSearchTab>> mpd_search_tabs_;

    // ADR-0188: server working tabs — client-owned, temporary lists of
    // server tracks that are edited freely and played like the queue.
    // Long-term curation lives in MPD stored playlists instead.
    struct MpdListTab {
        persistence::ListDocument document;
        quick::MpdQueueModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
    };
    std::vector<std::unique_ptr<MpdListTab>> mpd_list_tabs_;
    [[nodiscard]] int mpdTabInsertionIndex();
    MpdListTab* addMpdListTab(persistence::ListDocument document, bool select);
    [[nodiscard]] MpdListTab* mpdListTabForWidget(QWidget* widget) const;
    [[nodiscard]] MpdListTab* currentMpdListTab() const;
    void closeMpdListTab(MpdListTab* tab);
    void refreshMpdListTabChrome(MpdListTab& tab);
    void markMpdListTabDirty(MpdListTab& tab);
    [[nodiscard]] static QString mpdListTabLabel(const MpdListTab& tab);
    [[nodiscard]] bool isActiveMpdListTab(const MpdListTab& tab) const;
    void refreshActiveMpdListTab();
    void showMpdListTrackMenu(MpdListTab& tab, const QPoint& position);
    // ADR-0190: every MPD-side tab is a destination for a selection of
    // server tracks — the visible one by default, any other by name.
    struct MpdTabTarget {
        enum class Kind { queue, working, playlist };
        Kind kind{Kind::queue};
        QString label;
        MpdListTab* working{nullptr};
        QString playlist;
    };
    enum class MpdSendMode { append, insert_next, replace };
    [[nodiscard]] std::vector<mpd::Track> mpdTracksFromSourceView(QAbstractItemView* source) const;
    [[nodiscard]] std::vector<MpdTabTarget> mpdTabTargets() const;
    [[nodiscard]] std::optional<MpdTabTarget> visibleMpdTabTarget() const;
    void sendTracksToMpdTab(const MpdTabTarget& target, std::vector<mpd::Track> tracks,
                            MpdSendMode mode);
    void sendMpdLibraryEntryToTab(const QModelIndex& index, MpdSendMode mode);
    void addSendToTabMenu(QMenu* menu, const std::function<std::vector<mpd::Track>()>& selection);
    void addCopyToServerListMenu(QMenu* menu, QTableView* source_view);
    void addCopyToWorkingTabMenu(QMenu* menu, QTableView* source_view);
    [[nodiscard]] std::vector<mpd::Track> selectedMpdViewTracks(QTableView* view) const;
    MpdListTab* createServerListTab(const QString& name, std::vector<mpd::Track> tracks);
    // Enter pressed before the debounced search finished: commit this
    // query as soon as its results arrive (ADR-0140).
    QString pending_mpd_search_commit_;
    // ADR-0189: the Playlists sidebar is a tree — playlists expand to their
    // tracks, like albums in the library.
    QTreeWidget* mpd_playlists_list_{nullptr};
    [[nodiscard]] QStringList mpdPlaylistNames() const;
    QMenu* mpd_playlists_menu_{nullptr};
    QTimer* mpd_playlists_refresh_timer_{nullptr};

    QAction* previous_action_{nullptr};
    QAction* play_pause_action_{nullptr};
    // QAction::setIcon cannot compare icons, so it fires changed on every
    // call — and each ActionChanged makes QToolButton::setDefaultAction add
    // another connection. The 30 Hz transport refresh must therefore only
    // touch the icon when the playing state actually flips.
    std::optional<bool> transport_icon_playing_;
    QAction* stop_action_{nullptr};
    QAction* next_action_{nullptr};
    QAction* connect_mpd_action_{nullptr};
    QAction* disconnect_mpd_action_{nullptr};
    QAction* duplicate_tab_action_{nullptr};
    QAction* pin_tab_action_{nullptr};
    QAction* save_tab_action_{nullptr};
    QAction* rename_tab_action_{nullptr};
    QAction* close_tab_action_{nullptr};
    QAction* play_selected_action_{nullptr};
    QAction* properties_action_{nullptr};
    QAction* convert_action_{nullptr};
    QAction* mpd_load_local_action_{nullptr};
    QAction* mpd_edit_tags_action_{nullptr};
    QAction* mpd_replaygain_action_{nullptr};
    QAction* mpd_convert_action_{nullptr};
    QToolButton* library_order_az_{nullptr};
    QToolButton* library_order_latest_{nullptr};
    QAction* remove_selected_action_{nullptr};
    QAction* undo_list_action_{nullptr};
    QAction* redo_list_action_{nullptr};
    TrackListFindBar* list_find_bar_{nullptr};
    PlaylistTransferBar* playlist_transfer_bar_{nullptr};
    QAction* export_playlist_action_{nullptr};
    LocalListEditBar* list_edit_bar_{nullptr};
    QMenu* sort_list_menu_{nullptr};
    QAction* reverse_list_action_{nullptr};
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
    QWidget* mpd_status_separator_{nullptr};
    QAction* mpd_repeat_action_{nullptr};
    QAction* mpd_random_action_{nullptr};
    QAction* mpd_single_action_{nullptr};
    QAction* mpd_consume_action_{nullptr};
    QAction* mpd_append_selection_action_{nullptr};
    QAction* mpd_add_next_selection_action_{nullptr};
    QAction* mpd_crop_selection_action_{nullptr};
    QToolButton* mpd_repeat_button_{nullptr};
    QToolButton* mpd_random_button_{nullptr};
    QToolButton* mpd_single_button_{nullptr};
    QToolButton* mpd_consume_button_{nullptr};
    QToolButton* mpd_replaygain_button_{nullptr};
    QActionGroup* mpd_replaygain_group_{nullptr};
    QMenu* mpd_priority_menu_{nullptr};
    QMenu* mpd_rate_menu_{nullptr};
    QSlider* volume_{nullptr};
    QToolButton* device_button_{nullptr};
    QMenu* device_menu_{nullptr};
    QActionGroup* device_group_{nullptr};
    QMenu* buffer_menu_{nullptr};
    QActionGroup* buffer_group_{nullptr};
    QMenu* tab_context_menu_{nullptr};
    QMenu* track_context_menu_{nullptr};
    QMenu* folder_context_menu_{nullptr};
    QMenu* mpd_library_context_menu_{nullptr};
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
    QAction* mpd_go_to_artist_action_{nullptr};
    QAction* mpd_go_to_album_action_{nullptr};
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
    MaterializedDialog discovery_dialog_follow_up_{MaterializedDialog::none};
    MaterializedDialog pending_dialog_kind_{MaterializedDialog::none};
    QString pending_dialog_document_;

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

    QAction* local_repeat_action_{nullptr};
    QAction* local_random_action_{nullptr};
    QAction* local_single_action_{nullptr};
    QAction* local_consume_action_{nullptr};
    std::vector<QToolButton*> local_mode_buttons_;
    QToolButton* local_replaygain_button_{nullptr};
    QActionGroup* local_replaygain_group_{nullptr};
    bool local_repeat_{false};
    bool local_random_{false};
    int local_single_{0};
    int local_consume_{0};
    QString local_replaygain_{QStringLiteral("off")};
    double local_rg_preamp_with_{0.0};
    double local_rg_preamp_without_{0.0};
    audio::PlaybackOrder playback_order_;
    QPersistentModelIndex playback_index_;
    QPersistentModelIndex queued_playback_index_;
    QPersistentModelIndex requested_playback_index_;
    bool consuming_row_{false};
    QString playback_document_id_;
    int playback_row_{-1};
    LocalTrackSource playback_source_;
    bool advance_pending_{false};
    // Gapless continuation upkeep: the last takeover count seen, the last
    // requested next path, and a throttle for re-requests after the engine
    // dropped or rejected a queue.
    quint64 last_chain_transitions_{0U};
    std::optional<LocalTrackSource> last_requested_next_;
    QElapsedTimer next_request_timer_;
    bool seeking_{false};
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
