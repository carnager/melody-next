// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/desktop_notifier.hpp"
#include "bench/engine_key.hpp"
#include "bench/engine_list_sync.hpp"
#include "bench/engine_playback.hpp"
#include "bench/lastfm_service.hpp"
#include "bench/local_list_model.hpp"
#include "bench/local_playback_service.hpp"
#include "bench/mpris_service.hpp"
#include "bench/remote_engines.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/engine/remote_file_work.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "uicommon/list_persistence_service.hpp"
#include "uicommon/track_view_layout.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPersistentModelIndex>
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
#include <string>
#include <vector>

// A window's widgets, known here only by name: the workspace holds which
// view shows a list and which panel shows a library, never what they are.
class QTableView;

namespace trackknife::bench {

class LocalLibraryPanel;
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
// any of the window. Both the widgets window and the Qt Quick one are drawn
// over it, so each behaviour is written once.
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


    struct ListTab {
        persistence::ListDocument document;
        LocalListModel* model{nullptr};
        QTableView* view{nullptr};
        ui::TrackViewLayout view_layout;
        QByteArray preserved_view_layout;
        bool view_layout_persistence_protected{false};
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
    // Last explicitly played local list; transport stop does not release it.
    QString active_local_list_id_;
  public:
    // The engines, by key; the parts of one, null when absent.
    [[nodiscard]] EngineLink* link(const EngineKey& key) const;
    [[nodiscard]] EnginePlayback* playbackOf(const EngineKey& key) const;
    [[nodiscard]] CatalogueSource* catalogueOf(const EngineKey& key) const;
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
    // The lists as the workspace starts: those saved, or one to begin with.
    void restoreLists(std::vector<persistence::ListDocument> documents);
    // What is saved of each open list, in the order they are shown.
    [[nodiscard]] std::vector<persistence::ListDocument> collectDocuments();
    [[nodiscard]] std::vector<persistence::TrackViewPreset> collectTrackViewLayouts();
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

    // Edits. An edit made here is saved, and -- to the list playing -- told
    // to its engine, whose queue it is. A change the engine made is saved and
    // never sent back.
    void markTabDirty(ListTab& tab);
    void takeEngineChange(ListTab& tab);
    // The list playback was last started from.
    void setActiveLocalList(const QString& id);
    // Rows of one list into another -- moved or copied -- as the target's
    // engine sees their files; a move between two lists undoes as one step.
    bool transferRows(ListTab* source_tab, LocalListModel* source_model, const EngineKey& from,
                      bool dynamic, std::vector<int> rows, const QString& target_id, bool move,
                      int insertion_row);
    [[nodiscard]] bool canReplayCrossTabMove(bool undo);
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
    // Its rows, covers and the engine's requests brought up to date; true
    // when the rows were replaced.
    bool syncUpNextModel();
    void persistUpNext();
    void restoreUpNext();

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
