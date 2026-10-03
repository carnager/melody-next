// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/engine_key.hpp"
#include "bench/engine_launcher.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/local_list_model.hpp"
#include "bench/local_playback_service.hpp"
#include "bench/mpris_service.hpp"
#include "bench/output_profile_store.hpp"
#include "bench/remote_mount.hpp"
#include "bench/remote_engines.hpp"
#include "trackknife/audio/local_playback.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/track_view_layout.hpp"
#include "workspace/convert_job.hpp"
#include "workspace/tagger_session.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A window's widgets, known here only by name: the workspace holds which
// view shows a list and which panel shows a library, never what they are.
class QTableView;

namespace trackknife::bench {

class LocalLibraryPanel;
class LibraryBrowser;
enum class LocalLibraryAction;
class WorkspaceView;

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


// ADR-0220: the workspace a window shows -- the engines it reaches, the lists
// open from them, what plays and waits to, and the work under way -- without
// any of the window, so each behaviour is written once, apart from how it is
// drawn.
//
// For now it holds the state the widgets window kept; that window's logic
// moves here next, a part at a time, and the window becomes what draws it.
class Workspace final : public QObject {
    Q_OBJECT

  public:
    explicit Workspace(QObject* parent = nullptr);
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    ~Workspace() override;

    // The window drawing it; set once, before anything is asked.
    void setView(WorkspaceView* view) { view_ = view; }
    [[nodiscard]] WorkspaceView* view() const { return view_; }


    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
        QByteArray preserved_view_layout;
        bool view_layout_persistence_protected{false};
        // What the last successful save wrote of this list -- its content
        // revision and header; none, not saved yet. Unchanged since, a save
        // writes its header only.
        struct Saved {
            std::uint64_t revision{0};
            std::string name;
            persistence::ListKind kind{persistence::ListKind::scratch};
            bool pinned{false};
            bool dirty{false};
            std::string engine;
            friend bool operator==(const Saved&, const Saved&) = default;
        };
        std::optional<Saved> saved;
    };
    struct CrossTabMoveEdit {
        QString source_id;
        QString target_id;
        std::vector<LocalTrackRow> source_before;
        std::vector<LocalTrackRow> source_after;
        std::vector<LocalTrackRow> target_before;
        std::vector<LocalTrackRow> target_after;
        bool applied{true};
    };
    // ADR-0233: a file moved here is followed in the engines' lists too --
    // those this window has open and those it has not. Kept in Settings until
    // each engine has taken it, so one that is away catches up when it is
    // back.
    struct PendingRelocation {
        std::string from;
        std::string to;
        // The engines told, by key; "*" is every engine elsewhere -- what an
        // older release's "the remote has it" means now.
        std::set<QString> done;
        [[nodiscard]] bool doneFor(const EngineKey& engine) const {
            return done.contains(engine.text()) || (!engine.isLocal() && done.contains("*"));
        }
    };
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
    struct ArtworkJob {
        QString key;
        std::string raw_path;
        // Asked for the cover when the file is on its machine, not this one:
        // a remote tab's (ADR-0227). Unset, the file is read here.
        std::shared_ptr<engine::Catalogue> engine;
    };
    struct ArtworkOutcome {
        QString key;
        QImage image;
        // The engine could not be asked -- unreachable, say -- which is not
        // the same as "this album has no cover", and is not remembered as it.
        bool failed{false};
    };
    struct SeenEngine {
        QString status;
        QString entry;
        std::uint64_t queue_revision{0};
    };
    // ADR-0234: one engine this window reaches -- this computer's, which is
    // always there, or a remote configured in Settings -- and everything the
    // window keeps for it. Code that needs an engine asks for its link.
    struct EngineLink {
        EngineKey key;
        // How Settings name it; empty for this computer's.
        RemoteEngineSetting setting;
        // The password it was connected with. An empty one in `setting`
        // means this computer's, which Settings may have changed since.
        QString password;
        std::unique_ptr<CatalogueSource> catalogue;
        EnginePlayback* playback{nullptr};
        LocalLibraryPanel* library{nullptr};
        // Its library as a window browses it, once one does.
        QPointer<LibraryBrowser> browser;
        // What it was last seen doing, to tell a start elsewhere.
        SeenEngine seen;
        // A move is being told to it.
        bool relocating{false};
        // Its last list.all, for the lists pane; empty, not known.
        std::optional<std::vector<protocol::Json>> lists;
        QString lists_error;
        // ADR-0237: the file tools' reads, scans and writes through it, and
        // whether it does them -- asked each time it connects, off this
        // thread. Until it has said so, the tools do the work themselves.
        std::shared_ptr<engine::RemoteFileWork> file_work;
        bool does_file_work{false};
    };
    // ADR-0237: file work engines could neither finish nor roll back after a
    // crash, as each reports it -- shown with this window's own, once.
    struct EngineInterruption {
        core::StableId id;
        std::string raw_path;
        QString detail;
        bool move{false};
    };

    std::vector<std::unique_ptr<ListTab>> list_tabs_;
    QHash<QString, QByteArray> restored_track_view_layouts_;
    MprisService* mpris_{nullptr};
    // The cover file given to the desktop, and which album it is.
    QString desktop_cover_key_;
    QString desktop_cover_path_;
    DesktopNotifier* notifier_{nullptr};
    ui::ListPersistenceService* persistence_{nullptr};
    std::filesystem::path database_path_;
    // This computer's first.
    std::vector<std::unique_ptr<EngineLink>> engines_;
    // ADR-0233: this window's lists, on the engines that own their files.
    EngineListSync* list_sync_{nullptr};
    std::vector<PendingRelocation> pending_relocations_;
    // The engine the transport follows: the one the playing tab belongs to.
    // One engine plays at a time, so this is also the one that may.
    EnginePlayback* transport_{nullptr};
    // The connection Up Next was filled from, while it holds anything: its
    // asks are files on that engine's machine.
    EngineKey up_next_engine_{EngineKey::local()};
    // The entry the engine last reported. The engine advances its own queue,
    // so without following it the highlighted row would stay on whatever was
    // double-clicked while something else played.
    QString engine_entry_;
    // The request order last stated to the engine, so an unchanged panel does
    // not re-send it on every refresh; none when it is not known what the
    // engine holds -- after a handover, a reconnect or a start -- so even an
    // empty Up Next is stated then. An empty order is a statement too: the
    // engine keeps the asks it saved until it is told otherwise.
    std::optional<QString> engine_requests_;
    // The ReplayGain mode the playing engine last reported, so a change made
    // on it elsewhere is told from its default and from this window's own.
    std::optional<audio::ReplayGainMode> engine_replay_gain_;
    // The last entry the engine reported consuming, so one drop is mirrored
    // once however often the state is sampled.
    QString engine_consumed_;
    // The queue last pushed to the engine, and the engine revision that
    // produced. Together they answer "is what I am showing what the engine
    // holds, and if not, who changed it".
    QString engine_queue_;
    quint64 engine_queue_revision_{0};
    // The last ask for the engine's queue, per purpose: only its answer is
    // taken, an earlier one being out of date by the time it comes.
    quint64 engine_queue_asked_{0};
    quint64 engine_reattach_asked_{0};
    QTimer* persistence_timer_{nullptr};
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
    std::vector<EngineInterruption> engine_interruptions_;
    // This computer's move destinations as last loaded, for offering them to
    // an engine elsewhere through its mount.
    std::vector<persistence::SavedDestinationProfile> local_destinations_;
    // Layout hand-overs to engines, one at a time and in order.
    QThreadPool layout_pushes_;
    // Ratings read and stored, one at a time.
    QThreadPool catalogue_work_;
    // Paths opened before the asynchronous list restore finishes are queued
    // and flushed into the initial tab once it exists.
    std::vector<std::string> pending_open_paths_;
    bool lists_restored_{false};
    QString local_replaygain_{QStringLiteral("off")};
    double local_rg_preamp_with_{0.0};
    double local_rg_preamp_without_{0.0};
    std::optional<ListTab> detached_playback_;
    LocalListModel* up_next_local_model_{nullptr};
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
    QString lastfm_user_;
    // ADR-0253: the tkq-1 rules last seen in the definitions, so that one
    // gone is told from another window's that this one never knew.
    std::optional<QSet<QString>> known_rules_;
    void syncContinuations(EngineLink& engine);
    // Shows what `engine`'s tabs continue with as it says, and keeps its
    // copies of the rules in step.
    void followContinuations(EngineLink& engine);
    void rememberContinuationRules();
    // Last explicitly played local list; transport stop does not release it.
    QString active_local_list_id_;
  public:
    // Opens the workspace: this computer's engine connected, the saved lists
    // restored and the remote engines connected; then the window is told.
    void start();
    // The engines, by key; the parts of one, null when absent.
    [[nodiscard]] EngineLink* link(const EngineKey& key) const;

    // ADR-0237: what a tag editor is given for files `work` holds: where it
    // applies tags, moves files and keeps its choices, and how this
    // workspace follows what it changed.
    struct TaggerOpening {
        TaggerServices services;
        ArtworkWritePlanApplierFactory artwork_applier;
        ArtworkApplyObserver artwork_observer;
        // The engine whose files they are, whose destinations it moves to.
        EngineKey engine;
    };
    [[nodiscard]] static std::span<const std::string_view> taggerFields();
    [[nodiscard]] TaggerOpening taggerServices(std::shared_ptr<engine::RemoteFileWork> work);
    // The engine that does the file work for `engine`'s files, if one does.
    [[nodiscard]] std::shared_ptr<engine::RemoteFileWork> fileWorkOf(const EngineKey& engine) const;
    // The engine link whose file-work connection this is.
    [[nodiscard]] const EngineLink* linkOfWork(const engine::RemoteFileWork* work) const;
    // ADR-0156: the rows of a list a file tool reads, one at a time.
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(ListTab& tab, std::vector<QPersistentModelIndex> rows);
    [[nodiscard]] MetadataPropertiesSourceReader
    selectionSourceReader(LocalListModel* model, std::vector<QPersistentModelIndex> rows,
                          std::optional<std::vector<LocalTrackRow>> snapshot = std::nullopt);
    [[nodiscard]] MetadataApplyObserver metadataApplyObserver();
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
    // Naming layouts, and the move destinations of `destinations_of`; with
    // every engine's destinations as places for the manager (ADR-0237).
    [[nodiscard]] OutputProfileStore
    buildOutputProfileStore(const EngineKey& destinations_of = EngineKey::local());
    // What a file tool changed, followed by the lists, the library and the
    // queue.
    void applyCommittedMetadata(const operations::MetadataCommitResult& result);
    void applyCommittedCueReplayGain(const operations::CueReplayGainCommitResult& result);
    void applyCommittedLoudnessSidecar(const operations::LoudnessSidecarCommitResult& result);
    void applyCommittedRelocation(const operations::FilePublicationCommitResult& result);
    void applyCommittedPublicationMetadata(const operations::FilePublicationCommitResult& result,
                                           const metadata::MetadataDocument& document);
    // An engine elsewhere moved a file: its own tabs follow at its paths,
    // everything of this computer's at `here`, the move seen through the
    // mount, when it is.
    void applyEngineRelocation(const EngineKey& engine,
                               const operations::FilePublicationCommitResult& result,
                               const operations::FilePublicationCommitResult* here);
    // This computer's library, looked up again.
    void refreshLocalLibrary();
    // What the converter is given for rows of a list (ADR-0237 stage 6): a
    // remote tab's files where this computer has them, else fetched from
    // their engine; those it cannot reach are counted, not given.
    struct ConvertOpening {
        std::vector<ConvertDialogItem> items;
        std::size_t unreachable{0U};
    };
    [[nodiscard]] ConvertOpening convertItems(LocalListModel& model, const EngineKey& engine,
                                              std::vector<int> rows);
    [[nodiscard]] ConvertProfilesLoader convertProfiles();
    [[nodiscard]] ConvertPresetStore convertPresets();
    [[nodiscard]] EnginePlayback* playbackOf(const EngineKey& key) const;
    [[nodiscard]] CatalogueSource* catalogueOf(const EngineKey& key) const;

    // ADR-0253: a list continues with a dynamic playlist's tkq-1 rule once
    // it would end. The rules it can continue with, by identity and name.
    struct ContinuationChoice {
        QString rule_id;
        QString name;
    };
    [[nodiscard]] std::vector<ContinuationChoice> continuationChoices() const;
    // The rule `tab` continues with, as its engine keeps it; none, nothing.
    [[nodiscard]] std::optional<EnginePlayback::Continuation>
    continuationOf(const ListTab& tab) const;
    // Continues `tab` with the rule `rule_id`, or with an empty one ends it.
    void setContinuation(const ListTab& tab, const QString& rule_id);
    // The rules were saved: the engines' copies are brought in step, and a
    // rule deleted or made a Last.fm one ends the lists it continued.
    void refreshContinuations();
    // An open list, by its document's identity -- or the list that goes on
    // playing after its tab was closed.
    [[nodiscard]] ListTab* tabForDocument(const QString& document_id);
    [[nodiscard]] ListTab* tabForDocument(const core::StableId& document_id);
    // Saves the lists a moment from now.
    void schedulePersist();
    // Saves them now; `wait` until it is done, as at quitting.
    void persistNow(bool wait);

    // The lists. One is added with its rows as its document holds them, and
    // the window is asked to show it; its tags and covers are looked for.
    ListTab* addList(persistence::ListDocument document, bool select);
    // A list with its rows already made from its items -- made off the
    // window's thread, as restoring does.
    struct PreparedList {
        persistence::ListDocument document;
        std::vector<LocalTrackRow> rows;
    };
    ListTab* addList(PreparedList prepared, bool select);
    // The stored lists as rows, for a worker thread: the slow part of a start.
    [[nodiscard]] static std::vector<PreparedList>
    prepareLists(std::vector<persistence::ListDocument> documents);
    // Rows a search found (ADR-0153, ADR-0227), put in a tab of the engine
    // whose library they are: the current one if it is, else that engine's
    // first, else (for another engine) its own tab; or a new tab.
    void placeFoundRows(const QString& name, std::vector<LocalTrackRow> rows,
                        LocalLibraryAction action, const EngineKey& engine);
    // A dynamic playlist's result as a tab of its own (ADR-0145), laid out
    // flat as it was shown: to play from (not shown), or as an editable
    // snapshot (shown).
    ListTab* openDynamicResult(const QString& name, std::vector<LocalTrackRow> rows,
                               const EngineKey& engine, bool show);
    // The playing track marked in rows shown outside the lists -- a dynamic
    // playlist's result -- when the list `playback_context` plays, the same
    // occurrence of it; otherwise the first.
    void markPlaying(LocalListModel& model, const QString& playback_context);
    // What is dragged, as a drop needs it: rows of a list (or of a dynamic
    // playlist's result, which are only ever copied), or files -- paths
    // given, or resolved from a library's entries -- of `engine`.
    struct Dragged {
        ListTab* from_tab{nullptr};
        LocalListModel* from_model{nullptr};
        bool dynamic{false};
        std::vector<int> rows;
        std::vector<std::string> paths;
        // Files still to be found: a library's selection.
        std::function<void(std::function<void(std::vector<std::string>)>)> resolve;
        // That selection's entries, for Up Next, which takes tracks.
        QPointer<LibraryBrowser> library;
        std::vector<persistence::LibraryEntry> entries;
        EngineKey engine{EngineKey::local()};
        [[nodiscard]] bool isRows() const { return from_model != nullptr && !rows.empty(); }
        [[nodiscard]] bool isFiles() const { return !paths.empty() || static_cast<bool>(resolve); }
    };
    // Dropped on a list before `insertion_row` (-1: the end): its own rows
    // reordered, another's moved there (copied when `copy`, or from a
    // dynamic result), files added. False when it cannot take them.
    bool dropOnList(Dragged dragged, ListTab& target, int insertion_row, bool copy);
    // Dropped where no list is: a new list of them, shown.
    bool dropOnNewList(Dragged dragged, bool copy);
    // Dropped on Up Next before `position` (-1: the end): tracks, not
    // folders or files from elsewhere.
    bool dropOnUpNext(Dragged dragged, int position);
    // Dropped on a list in the lists pane: open here, as on its tab; not
    // open, copied into it once it is.
    bool dropOnEngineList(Dragged dragged, const EngineKey& engine, const QString& id);
    // Files of `files_engine` added to the list `id` before `row` (-1: the
    // end), as that list's engine sees them.
    void addDroppedPaths(const QString& id, const EngineKey& files_engine,
                         std::vector<std::string> paths, int row = -1);
    // An imported playlist, as a new saved list of this computer's.
    void addImportedList(std::vector<LocalTrackRow> rows, const QString& name);
    // The workspace database and its settings copied beside `path`: the
    // database there, the settings in `path`.settings.ini; said when done.
    void backupWorkspace(const QString& path);
    // `path` restored at the next start, with the settings backed up beside
    // it; the current database kept for rollback.
    static void scheduleWorkspaceRestore(const QString& path);
    // The lists as the workspace starts: those saved, or one to begin with.
    void restoreLists(std::vector<PreparedList> documents);
    // What a start does once the lists are restored.
    void restoredWorkspace(const QString& error);
    // What is saved of each open list, in the order they are shown.
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();
    // One list as it is stored, with its items; and without them.
    [[nodiscard]] persistence::ListDocument documentOf(const ListTab& tab) const;
    // ADR-0256: a tab's entries as its engine stores them -- with what a
    // queue entry needs that the engine cannot read, so the list plays by
    // reference.
    [[nodiscard]] static std::vector<persistence::EngineListItem> engineItemsOf(const ListTab& tab);
    // The sync's way of reading them, by list.
    [[nodiscard]] EngineListSync::ItemsOf engineItems();
    [[nodiscard]] static persistence::ListDocument headerOf(const ListTab& tab);
    [[nodiscard]] static ListTab::Saved savedStateOf(const ListTab& tab);
    // Every list for a save: with items where they changed since the last.
    [[nodiscard]] std::vector<persistence::ListDocumentWrite> collectWrites();
    [[nodiscard]] std::vector<persistence::TrackViewPreset> collectTrackViewLayouts();
    // A presentation's columns as a new list shows them.
    [[nodiscard]] static ui::TrackViewLayout defaultTrackViewLayout(
        ui::TrackViewPresentation presentation = ui::TrackViewPresentation::albums_side_artwork);
    // The columns a list was saved with, or the default when there are none
    // -- or they cannot be read, when what was saved is kept and not
    // overwritten.
    [[nodiscard]] ui::TrackViewLayout restoredTrackViewLayout(ListTab& tab);
    // A list's columns as chosen now -- what was saved is replaced -- and
    // one of them shown or hidden; the last shown is not hidden (false).
    void setTrackViewLayout(ListTab& tab, ui::TrackViewLayout layout);
    bool setColumnVisible(ListTab& tab, ui::TrackViewLayout layout, const QString& id,
                          bool visible);
    // ADR-0233: another client's version of a list open here. A row that is
    // the same entry of the same file keeps what is already known of it.
    void adoptEngineList(const persistence::ListDocument& document);
    // The open lists of one engine, in the order shown, by id and name.
    [[nodiscard]] std::vector<std::pair<QString, QString>> listTargets(const EngineKey& engine) const;
    // An engine's list opened here -- or shown, when it already is -- and
    // then whatever is to be done with it.
    void openEngineList(const EngineKey& key, const QString& id, std::function<void()> then = {});
    // Files on this computer into a local list: the one shown when it is
    // one, else the first there is, else a new one.
    void openLocalPaths(std::vector<std::string> raw_paths);
    // A list closed: gone, unless it is the one playing with Up Next still
    // waiting, which plays on detached; never none at all.
    void closeList(ListTab& tab);

    // A named list made, shown and saved; none without a name.
    ListTab* createList(const QString& name);
    // A copy of a list: "<name> copy", unpinned, modified, laid out alike.
    ListTab* duplicateList(const ListTab& tab);
    void togglePinned(ListTab& tab);
    // Saved: a working list takes `name` and becomes a named one.
    void saveList(ListTab& tab, const QString& name);
    void renameList(ListTab& tab, const QString& name);

    // Edits. An edit made here is saved, and -- to the list playing -- told
    // to its engine, whose queue it is. A change the engine made is saved and
    // never sent back.
    void markTabDirty(ListTab& tab);
    // Rows taken out of a list, as one step to undo.
    void removeRows(ListTab& tab, std::vector<int> rows);
    void takeEngineChange(ListTab& tab);
    // The list playback was last started from.
    void setActiveLocalList(const QString& id);
    // Rows of one list into another -- moved or copied -- as the target's
    // engine sees their files; a move between two lists undoes as one step.
    bool transferRows(ListTab* source_tab, LocalListModel* source_model, const EngineKey& from,
                      bool dynamic, std::vector<int> rows, const QString& target_id, bool move,
                      int insertion_row);
    // The rows into a new list named `name`, of the same engine; the list,
    // or null when nothing went.
    ListTab* transferRowsToNewList(ListTab* source_tab, LocalListModel* source_model,
                                   const EngineKey& from, bool dynamic, std::vector<int> rows,
                                   bool move, const QString& name);
    [[nodiscard]] bool canReplayCrossTabMove(bool undo);
    // A list's last edit undone, or redone -- a move between two lists as
    // one step. False when there was none.
    bool replayListEdit(ListTab* tab, bool undo);
    struct HistoryTexts {
        QString undo;
        QString redo;
        bool can_undo{false};
        bool can_redo{false};
        bool editable{false};
    };
    [[nodiscard]] HistoryTexts historyTexts(const ListTab* tab);
    bool replayCrossTabMove(bool undo);

    // ADR-0227, ADR-0234: paths moving from one engine's list to another's,
    // as the other engine sees them (RemoteMount). What cannot be -- not
    // reachable here, or not in the remote's library -- is left out, and a
    // message says how much and why.
    [[nodiscard]] std::optional<std::string>
    crossEnginePath(const std::string& path, const EngineKey& from, const EngineKey& to) const;
    [[nodiscard]] std::vector<std::string>
    crossEnginePaths(std::vector<std::string> paths, const EngineKey& from, const EngineKey& to);
    [[nodiscard]] std::vector<LocalTrackRow>
    crossEngineRows(std::vector<LocalTrackRow> rows, const EngineKey& from, const EngineKey& to);
    [[nodiscard]] std::vector<std::string> rootsOf(const EngineLink& engine) const;
    [[nodiscard]] RemoteMount mountOf(const EngineLink& engine) const;
    // How an engine is named to the user: "this computer", or its name.
    [[nodiscard]] QString engineName(const EngineKey& engine) const;
    // This computer's engine and its parts.
    [[nodiscard]] EngineLink& localEngine() const { return *engines_.front(); }
    [[nodiscard]] CatalogueSource* localCatalogue() const { return localEngine().catalogue.get(); }
    [[nodiscard]] EnginePlayback* localPlayback() const { return localEngine().playback; }
    // ADR-0226: this computer's engine outlives the window, so a rebuilt or
    // updated one keeps running the old program until it is restarted --
    // done here, at once when nothing plays, else when playback stops.
    void renewOutdatedLocalEngine();
    bool engine_renewal_pending_{false};
    // Quitting: no engine is to be started again, and this computer's stops.
    void retireEngines();
    // The link a connection belongs to; null for none of the workspace's.
    [[nodiscard]] EngineLink* linkOf(const EnginePlayback* playback) const;
    // An engine's own list: its first, or one made for it, named after it.
    [[nodiscard]] ListTab* engineTab(EngineLink& engine);
    [[nodiscard]] ListTab* remoteQueueTab();
    [[nodiscard]] EngineLink* remoteEngine() const {
        for (const auto& engine : engines_) {
            if (!engine->key.isLocal()) {
                return engine.get();
            }
        }
        return nullptr;
    }

    // The engines connected: this computer's first, as the workspace
    // starts; then each remote in Settings -- the `first` of them owning the
    // lists an older release made for "the remote".
    void connectLocalEngine();
    void connectRemoteEngine(const RemoteEngineSetting& setting, bool first);
    // Settings changed: engines no longer listed let go, new ones connected.
    void syncRemoteEngines();
    // Settings were saved: the engines, this computer's engine (restarted
    // when how it is shared changed from `before`) and playback follow.
    void settingsSaved(const LocalEngineSharing& before);
    void disconnectEngine(const EngineKey& key);
    // ADR-0234: a remote's placeholder key replaced by the id it gave.
    void adoptEngineIdentity(EngineLink& engine);
    // Ratings (ADR-0179), read from and stored on the engine whose library
    // holds the tracks, by their hashes; album ratings by the album's.
    void refreshRatings();
    [[nodiscard]] bool canRate(const EngineKey& engine) const;
    void rate(const EngineKey& engine, const QStringList& hashes, bool album, unsigned rating);
    // A rating an engine stored, shown in its lists.
    void adoptEngineRating(const EngineKey& engine, const QString& hash, unsigned rating);
    // ADR-0237: whether an engine does the file tools' work, asked each time
    // it connects; one that does is handed this workspace's naming layouts.
    void watchFileWork(EngineLink& link);
    void pushLayouts(std::vector<core::StableId> removed = {});
    // ADR-0233: a file moved here, told to every engine's lists; kept in
    // Settings until each has taken it.
    void queueEngineRelocation(const std::string& from, const std::string& to);
    void storePendingRelocations() const;
    void loadPendingRelocations();
    void flushEngineRelocations();

    // Playback (ADR-0220, ADR-0226): the engine owns the queue, the modes,
    // the order and up-next; the workspace says what to play and follows
    // what it does.
    // True while the engine followed is connected; nothing plays otherwise.
    [[nodiscard]] bool playingOnEngine() const;
    void playRow(ListTab& tab, int row);
    void togglePlayPause();
    void seekToMs(qint64 position_ms);
    // The modes and ReplayGain, saved, and told to the engine.
    void saveLocalPlaybackModes();
    void loadLocalPlaybackModes();
    // The ReplayGain mode this window's setting means now ("auto" follows
    // shuffle), and that mode given to every engine it reaches.
    [[nodiscard]] audio::ReplayGainMode resolvedReplayGain() const;
    void syncReplayGain();
    void applyLocalPlaybackModes();
    // Makes `playback` the one followed, stopping the other when
    // `stop_other`: one engine plays at a time.
    void followPlayback(EnginePlayback* playback, bool stop_other = true);
    // Another client started the engine not followed: followed now.
    void followIfStartedElsewhere(EnginePlayback* playback);
    void rememberEngineState(EnginePlayback* playback);
    // Adopts whatever the engine is already playing -- after a restart of
    // the window, the music is still going.
    void reattachToEngine();
    void reattachToQueue(std::vector<LocalTrackRow> rows);
    void adoptEngineRow(ListTab& tab, int row, const core::StableId& entry);
    // The engine's queue and the playing list kept in step, both ways; and
    // Up Next stated to it.
    void syncEngineQueue();
    void syncEngineRequests();
    void adoptEngineQueue();
    void adoptEngineQueue(std::vector<LocalTrackRow> held);
    // The playing entry's row in `tab`, or -1.
    [[nodiscard]] int resolvePlaybackRow(const ListTab* tab) const;
    // The row for an entry the engine plays, wherever it is held.
    [[nodiscard]] const LocalTrackRow* playingRow(const QString& entry);

    // Up Next: asks, played before the list goes on. They are one engine's
    // files at a time (ADR-0227), each an occurrence of its own (ADR-0221).
    void enqueueLocalRequests(std::vector<LocalTrackRow> rows, int position = -1,
                              const EngineKey& engine = EngineKey::local());
    // 0 clears, 1 removes `row`, otherwise moves it to `destination`.
    void editUpNext(int operation, int row = -1, int destination = -1);
    // The rows `selected` removed (1), moved up (2), moved down (3), or to
    // an insertion `destination`.
    void editUpNextRows(std::vector<bool> selected, int operation, int destination = -1);
    // An ask played now, before the others; all of them dropped and the
    // list gone back to.
    void playUpNextRow(int row);
    // Up Next's last edit taken back.
    void undoUpNext();
    void returnToList();
    // A list's `rows` asked for: at the front (0), the end (-1) or `position`.
    void enqueueRows(const ListTab& tab, std::vector<int> rows, int position);
    // Its rows, covers and the engine's requests brought up to date; true
    // when the rows were replaced.
    bool syncUpNextModel();
    void persistUpNext();
    void restoreUpNext();

    // What things are called, as every window says it (workspace_texts.cpp).
    [[nodiscard]] QString emptyListTitle(const EngineKey& engine) const;
    [[nodiscard]] QString emptyListHint(const EngineKey& engine) const;
    struct TabChrome {
        QString text;
        QString tooltip;
        QString accessible_name;
        // The list playback was last started from: a dot on its tab.
        bool playing{false};
        // Its files are an engine elsewhere's: an icon says so.
        bool remote{false};
        // ADR-0253: the rule it continues with; empty, none.
        QString continues;
    };
    [[nodiscard]] TabChrome tabChrome(const ListTab& tab) const;
    struct Summary {
        QString text;
        QString tooltip;
    };
    // The selected `rows` of a list, as the status bar says them.
    [[nodiscard]] Summary selectionSummary(const ListTab* tab, const std::vector<int>& rows) const;
    struct UpNextHeading {
        QString status;
        QString status_tooltip;
        QString back;
        QString back_tooltip;
        bool back_enabled{false};
        bool can_undo{false};
    };
    [[nodiscard]] UpNextHeading upNextHeading() const;
    struct ModeText {
        QString text;
        QString tooltip;
        bool checked{false};
        bool oneshot{false};
    };
    struct ModeTexts {
        ModeText repeat;
        ModeText random;
        ModeText album_random;
        ModeText single;
        ModeText consume;
        QString replaygain;
        QString replaygain_tooltip;
        bool replaygain_active{false};
    };
    [[nodiscard]] ModeTexts modeTexts() const;
    // ReplayGain's modes, by label and the value saved and sent.
    [[nodiscard]] static std::vector<std::pair<QString, QString>> replayGainModes();

    // The transport and the modes, as the controls ask for them; nothing
    // when no engine is followed.
    void previous();
    void next();
    void stop();
    void setVolume(int percent);
    void selectOutput(const std::string& id);
    void setOutputDevice(const std::optional<std::string>& target);
    void refreshOutputs();
    void setRepeat(bool on);
    void setRandom(bool on);
    void setAlbumRandom(bool on);
    void cycleSingle();
    void cycleConsume();
    void setReplayGain(const QString& mode);

    // What the engine followed reports, taken in: modes it changed, rows it
    // consumed, the Up Next ask it started, a queue changed elsewhere.
    void followEngineState(const EnginePlayback::State& state);
    // What plays, as the header says it. No title for the window means the
    // plain one; no cover entry, no cover.
    struct NowPlaying {
        QString title;
        QString context;
        QString tooltip;
        QString window_title{QStringLiteral("Trackknife")};
        QString cover_entry;
    };
    [[nodiscard]] NowPlaying nowPlaying(const EnginePlayback::State& state);
    // ADR-0226, ADR-0228: where it sounds. Taken from each state -- saying
    // what changed since the last -- and offered as a menu of the engine's
    // speakers and the chosen one's devices.
    struct OutputSummary {
        // Empty when there is no choice to show: the engine's own default.
        QString shown;
        QString tooltip;
        QString description;
        QString selected_output;
        bool menu_changed{false};
    };
    struct OutputMenu {
        struct Speaker {
            std::string id;
            QString label;
            QString tooltip;
            bool checked{false};
        };
        struct Device {
            std::optional<std::string> target;
            QString label;
            bool checked{false};
            bool enabled{true};
        };
        bool speakers_shown{false};
        std::vector<Speaker> speakers;
        QString devices_heading;
        std::vector<Device> devices;
    };
    [[nodiscard]] OutputSummary takeOutputs(const EnginePlayback::State& state);
    [[nodiscard]] OutputMenu outputMenu() const;
    [[nodiscard]] QString outputLabel(const EnginePlayback::State::Output& output) const;
    // The engine's playback buffer, by profile; Settings mirror it.
    struct PlaybackBufferPreference {
        QString profile;
        audio::PlaybackBufferDurationConfig config;
    };
    [[nodiscard]] static QString bufferProfileLabel(const QString& profile);
    [[nodiscard]] static PlaybackBufferPreference loadPlaybackBufferPreference();
    void configurePlaybackBuffer(const QString& profile, int capacity_ms, int start_threshold_ms);
    // Settings changed: the buffer and ReplayGain preamps told to the engine.
    void reloadPlaybackPreferences();
    QString selected_buffer_profile_{QStringLiteral("balanced")};
    std::vector<std::pair<std::string, std::string>> device_choices_;
    std::optional<std::string> selected_device_;
    std::optional<std::string> default_device_;
    bool selected_device_available_{true};
    // ADR-0228: the engine's outputs, as last reported.
    std::vector<EnginePlayback::State::Output> output_choices_;
    // Whether those are an engine elsewhere's, which names its own audio
    // differently.
    EngineKey output_choices_engine_{EngineKey::local()};
    // Whether an engine state has been seen, so the first one does not read
    // as the output changing.
    bool engine_output_seen_{false};

    // A library's requests handled: its entries into lists and Up Next,
    // searches kept as lists, ratings read again when it stores one.
    void attachLibrary(LibraryBrowser* browser);
    // `name`, for a new list: what it is called, else after its entries.
    void libraryAction(LibraryBrowser& browser, std::vector<persistence::LibraryEntry> entries,
                       LocalLibraryAction action, const QString& name = {});
    void libraryAddToList(LibraryBrowser& browser, std::vector<persistence::LibraryEntry> entries,
                          const QString& id);
    void librarySearchCommitted(LibraryBrowser& browser, const QString& query,
                                std::vector<LocalTrackRow> rows);
    [[nodiscard]] int insertionForNext(const ListTab& target);

    // Last.fm, signed in as this window's account: listens credited from
    // what the engine followed reports, unless it scrobbles itself.
    void startLastFm();
    void sampleLastFm(const EnginePlayback::State& state);
    // What plays, as MPRIS and the track-change notification show it.
    void publishDesktopState();
    // The play order as MPRIS names it: repeat and single together loop the
    // track ("Track"), repeat alone the list ("Playlist"), else "None"; and
    // shuffled is random or album random. Set from the desktop, the modes
    // follow and every engine is told (ADR-0135).
    [[nodiscard]] QString loopStatus() const;
    void setLoopStatus(const QString& status);
    [[nodiscard]] bool shuffled() const;
    void setShuffled(bool shuffled);
    // The playing album's cover as a file the desktop can read -- written
    // once per album into the cache, the last one removed -- or empty.
    [[nodiscard]] QString desktopCoverPath(const LocalTrackRow& track, const EngineKey& engine);

    // Files into lists: discovered from paths and folders (CUE sheets
    // expanded), probed for their tags a batch at a time, and -- for a list
    // on an engine elsewhere -- filled in from that engine's index.
    void startDiscovery(std::vector<std::string> raw_paths, QString target_document_id,
                        int insertion_row, bool replace_and_play = false);
    void enqueueUnprobedRows(ListTab& tab);
    void enrichRemoteRows(ListTab& tab);
    // Rows from paths a remote engine gave -- a drag from its library --
    // without looking for them on this computer, where they need not be.
    void insertRemotePaths(ListTab& tab, std::vector<std::string> raw_paths, int insertion_row);
    // Covers: asked for each album of a list, once, and cached.
    void syncArtwork(ListTab& tab);
    // An album's cover from the lists or the cache; fetched when neither has
    // it, arriving later through the same path as a list's.
    [[nodiscard]] QImage coverFor(const LocalTrackRow& track, const EngineKey& engine);
    void invalidateArtwork(const std::string& raw_path);

  private:
    void finishDiscovery();
    void pumpProbeQueue();
    void finishProbeBatch();
    void pumpArtworkQueue();
    void finishArtworkLoad();

    WorkspaceView* view_{nullptr};

};

} // namespace trackknife::bench
