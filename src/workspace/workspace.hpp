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
