// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/track_source.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/lists/list_view.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAbstractTableModel>
#include <QCache>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QSet>

class QTimer;
namespace trackknife::ui {
class ListPersistenceService;
}

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace trackknife::bench {

// Trackknife's local presentation has a real artwork/status column followed by
// independent metadata columns. Shared semantic roles still let the grouped
// delegate and queue view consume the model without owning local-file state.
enum LocalTrackColumn : int {
    local_artwork_column = ui::track_artwork_column,
    local_artist_column = ui::track_artist_column,
    local_track_number_column = ui::track_number_column,
    local_title_column = ui::track_title_column,
    local_album_column = ui::track_album_column,
    local_date_column = ui::track_date_column,
    local_length_column = ui::track_length_column,
    local_rating_column = ui::track_rating_column,
    local_play_count_column = ui::track_play_count_column,
    local_last_played_column = ui::track_last_played_column,
    local_column_count = ui::track_column_count,
};

// One row of a Trackknife working list: raw Linux path bytes plus the
// display metadata enriched by the background probe or restored from the
// persisted document.
// ADR-0153: probe-retained technical facts; absent until a probe ran
// (rows restored from a saved workspace fill on demand).
struct LocalTrackTechnicals {
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t bit_rate{0};

    friend bool operator==(const LocalTrackTechnicals&, const LocalTrackTechnicals&) = default;
};

struct LocalTrackRow {
    // ADR-0221: identity of this entry in its list, carried so a row stays
    // addressable as the list is reordered and so saving does not mint a new
    // identity on every pass. Fresh rows get one immediately; rows restored
    // from a document adopt the persisted value.
    //
    // Excluded from equality below for the same reason as the track itself:
    // comparisons here ask whether two rows describe the same track.
    core::StableId entry_id{core::StableId::random()};
    std::string raw_path;
    std::optional<std::string> logical_reference;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string date;
    std::string track_number;
    std::optional<std::int64_t> duration_ms;
    // The display columns above are a projection of this ordered, multi-value
    // document. The observed revision is retained only as stale-read evidence;
    // any future mutation must revalidate it immediately before commit.
    metadata::MetadataDocument metadata;
    std::optional<core::LocalSourceRevision> source_revision;
    std::optional<LocalTrackTechnicals> technicals{};
    // ADR-0179: Melody-compatible content-identity rating keys derived from
    // the display document, and the stored 0-10 rating loaded by hash. Like
    // artwork, this is display-cache state: it is recomputed/reloaded rather
    // than owned by the row, so equality deliberately ignores it.
    std::string rating_hash{};
    std::string album_rating_hash{};
    unsigned rating{0U};
    unsigned album_rating{0U};
    // ADR-0283: when the track, and its album's newest track, came into its
    // engine's library -- Unix seconds, 0 not known. Display state, as the
    // ratings are.
    std::int64_t added{0};
    std::int64_t album_added{0};
    // True once a probe ran or persisted metadata was restored; unprobed rows
    // fall back to their file name and are queued for enrichment.
    bool probed{false};
    // ADR-0282: what the list's view made of the row -- its expression
    // cells and group keys -- and for which view; dropped when the row
    // changes. Display state, so equality ignores it.
    std::shared_ptr<const lists::EvaluatedRow> evaluated{};
    std::uint64_t evaluated_view{0U};
    std::uint64_t evaluation_ticket{0U};
    // The engine's listening history of the row, fetched when its view reads
    // it (lists::history_field_names order); asked for, and arrived.
    std::optional<std::array<std::int64_t, 6>> history{};
    bool history_requested{false};
    bool history_loaded{false};

    friend bool operator==(const LocalTrackRow& left, const LocalTrackRow& right) {
        const auto salient = [](const LocalTrackRow& row) {
            return std::tie(row.raw_path, row.logical_reference, row.selection, row.segment,
                            row.title, row.artist, row.album, row.album_artist, row.date,
                            row.track_number, row.duration_ms, row.metadata, row.source_revision,
                            row.technicals, row.probed);
        };
        return salient(left) == salient(right);
    }
};

// ADR-0220 Phase 0: the type itself is audio::TrackSource, which is Qt-free so
// that playback state holding one can leave the widget layer. The alias keeps
// the established spelling at the several hundred call sites in src/bench.
using LocalTrackSource = audio::TrackSource;

class LocalListModel;

// ADR-0220 Phase 0: the engine's narrow view of a playing list. The model owns
// presentation; this exposes only what choosing the next track needs, so the
// advance rules can run without a widget in sight.
class LocalListPlaybackView final : public audio::PlaybackList {
  public:
    explicit LocalListPlaybackView(const LocalListModel& model) : model_(&model) {}

    [[nodiscard]] int row_count() const override;
    [[nodiscard]] int row_of_entry(const core::StableId& entry, int hint_row) const override;
    [[nodiscard]] audio::TrackSource source_at(int row) const override;

  private:
    const LocalListModel* model_;
};

// Table model over one Trackknife working list, implementing the shared
// album-grouped semantic role contract (uicommon/track_row_roles.hpp). Its
// seven-column Trackknife projection keeps artwork, artist, number, and title
// independently arrangeable. Rows hold raw path bytes; presentation uses the
// lossless escaped form from the core.
class LocalListModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    explicit LocalListModel(QObject* parent = nullptr);
    void setListeningHistoryService(ui::ListPersistenceService* service);
    void invalidateListeningHistory();

    void replaceRows(std::vector<LocalTrackRow> rows, bool remember = false, QString label = {});
    void appendPaths(std::vector<std::string> raw_paths, int insertion_row = -1);
    void appendRows(std::vector<LocalTrackRow> rows, int insertion_row = -1, bool remember = true);
    // ADR-0153: stores on-demand probed technicals onto every row of the
    // given physical source.
    void applyTechnicals(const std::string& raw_path, const LocalTrackTechnicals& technicals);
    // ADR-0179: applies stored ratings by content-identity hash to every row
    // whose track hash appears in the map.
    void applyRatings(const QHash<QString, unsigned>& ratings);
    // ADR-0268: of the files `checked`, those in `missing` are not there and
    // their rows show greyed; the rest are there again.
    void setMissing(const std::vector<std::string>& checked,
                    const std::vector<std::string>& missing);
    [[nodiscard]] bool isMissing(const std::string& raw_path) const {
        return missing_.contains(raw_path);
    }
    // Distinct track rating hashes across all rows, for a bulk store lookup.
    [[nodiscard]] QStringList ratingHashes() const;
    void removeRowIndexes(std::vector<int> rows, bool remember = true, QString label = {});
    bool applyPermutation(const std::vector<int>& order, QString label);
    [[nodiscard]] bool canUndo() const noexcept { return history_cursor_ > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return history_cursor_ < history_.size(); }
    [[nodiscard]] QString undoLabel() const;
    [[nodiscard]] QString redoLabel() const;
    bool undo();
    bool redo();
    void clearHistory();
    // Moves the given rows (ascending, deduplicated) as one block to
    // insertion_row, preserving their relative order.
    void reorderRows(std::vector<int> rows, int insertion_row);
    // Applies probe metadata to the row holding raw_path (hint first, then
    // search); returns false when the row no longer exists.
    bool applyMetadata(const std::string& raw_path, int hint_row, LocalTrackRow metadata);
    // Replaces one still-provisional whole-file row with one enriched row or
    // several logical chapter rows. The first row keeps the original model
    // position and any following rows are inserted directly after it.
    bool applyProbeRows(const std::string& raw_path, int hint_row, std::vector<LocalTrackRow> rows);
    // ADR-0259: the row at `row`, when it is still entry `entry`, takes what
    // its engine's library knows of its file; its place, identity and the
    // part of the file it plays stay. False when it is not that entry.
    bool describeRow(int row, const core::StableId& entry, LocalTrackRow described);
    // ADR-0268: the rows at these paths -- or under them, a folder; or,
    // `everything`, every row -- whose files changed, to be described again.
    // A CUE track is described by its sheet, and is left out.
    [[nodiscard]] std::vector<core::StableId> entriesAt(const std::vector<std::string>& raw_paths,
                                                        bool everything) const;
    // Refreshes every duplicate/logical occurrence of one verified physical
    // source while retaining annotation, segment, and sidecar layers.
    [[nodiscard]] core::Result<std::size_t>
    applyCommittedMetadata(const std::string& raw_path, const metadata::MetadataDocument& document,
                           const core::LocalSourceRevision& published_revision);
    // ADR-0139: refreshes segment-provenance ReplayGain projections after
    // a committed CUE sheet rewrite. Prefix matching addresses every
    // logical track of one sheet (album values), exact matching one track.
    struct CueReplayGainFieldUpdate {
        std::string display_name;
        std::string canonical_name;
        std::optional<std::string> value;
    };
    std::size_t applyCueReplayGain(const std::string& reference, bool prefix_match,
                                   const std::vector<CueReplayGainFieldUpdate>& fields);
    // ADR-0141: refreshes sidecar-provenance ReplayGain projections for
    // the rows matching one in-file logical identity after a committed
    // sidecar merge.
    struct SidecarRowIdentity {
        std::optional<int> stream_index;
        std::optional<int> subsong_index;
        std::optional<std::int64_t> start_sample;
        std::optional<std::int64_t> end_sample;
    };
    std::size_t applySidecarLoudness(const std::string& raw_path,
                                     const SidecarRowIdentity& identity,
                                     const std::vector<CueReplayGainFieldUpdate>& fields);
    // Advances every in-memory occurrence of one durably relocated physical
    // source while retaining logical identities and playback selection.
    [[nodiscard]] core::Result<std::size_t>
    applyCommittedRelocation(const std::string& source_raw_path, const std::string& target_raw_path,
                             const core::LocalSourceRevision& previous_revision,
                             const core::LocalSourceRevision& published_revision);
    // Marks the playing occurrence rendered by the shared delegate; an empty
    // path clears it.
    void setCurrentPath(std::string raw_path, int hint_row);
    void setCurrentSource(LocalTrackSource source, int hint_row);
    // The delegate's album-grouping identity for a row; artwork is keyed and
    // painted per group.
    [[nodiscard]] QString groupKey(int row) const;
    // "Disc 2" where a disc begins in an album of several (track_disc_start_role).
    [[nodiscard]] QString discStart(int row) const;
    // The same key for a row held anywhere: an album's cover is shared by it.
    [[nodiscard]] static QString groupKeyOf(const LocalTrackRow& track);
    [[nodiscard]] bool hasArtwork(const QString& key) const { return artwork_.contains(key); }
    void setArtwork(const QString& key, QImage image);
    [[nodiscard]] QImage artwork(const QString& key) const { return artwork_.value(key); }
    [[nodiscard]] const QHash<QString, QImage>& artworks() const noexcept { return artwork_; }
    [[nodiscard]] const std::vector<LocalTrackRow>& rows() const noexcept { return rows_; }
    // Goes up with every change to what the list holds -- rows, their order,
    // their tags -- so a save writes only lists that changed (and a ratings
    // or playing-marker refresh, which stores nothing, is not one).
    [[nodiscard]] std::uint64_t contentRevision() const noexcept { return content_revision_; }
    [[nodiscard]] std::string rawPath(int row) const;
    [[nodiscard]] LocalTrackSource source(int row) const;
    // Finds the row holding raw_path, preferring the hint row so duplicate
    // occurrences re-anchor to the same position after edits.
    [[nodiscard]] int rowOfPath(const std::string& raw_path, int hint_row) const;
    [[nodiscard]] int rowOfSource(const LocalTrackSource& source, int hint_row) const;
    // ADR-0221: resolve an entry identity to its current row. The hint makes
    // the common case -- the row has not moved -- a single comparison, the
    // same shape as rowOfSource above. Returns -1 when the entry is no longer
    // in this list, which is how a consumed or removed row reports itself now
    // that a QPersistentModelIndex is not silently tracking it.
    [[nodiscard]] int rowOfEntry(const core::StableId& entry, int hint_row) const;

    // ADR-0282: the view the list is drawn by. Its expression columns come
    // after the built-in ones; they are evaluated off the window's thread,
    // and blank until they are.
    void setListView(std::shared_ptr<const lists::CompiledListView> view);
    [[nodiscard]] const lists::CompiledListView* listView() const noexcept {
        return list_view_.get();
    }
    [[nodiscard]] int expressionColumnCount() const noexcept { return expression_columns_; }
    // A column's id -- built-in or expression -- and the column of an id; -1
    // when there is none.
    [[nodiscard]] QString columnId(int column) const;
    [[nodiscard]] int columnOf(const QString& id) const;
    // Grouped by an expression, and every row evaluated: the groups are
    // known. Until then the list is drawn ungrouped.
    [[nodiscard]] bool expressionGrouping() const noexcept;
    [[nodiscard]] bool expressionGroupsReady() const noexcept { return groups_ready_; }
    // Each row's copy for a worker to evaluate -- a sort by a column.
    [[nodiscard]] std::vector<lists::OwnedViewRow> viewRows() const;
    // ADR-0282: listening history for a view that reads it. The rows whose
    // history is wanted and not yet asked for -- now asked for -- with what
    // the engine looks them up by; what it answered, or that it could not;
    // and the files whose history changed, to be asked for again.
    struct HistoryRequest {
        core::StableId entry;
        persistence::LibraryHistorySource source;
    };
    [[nodiscard]] std::vector<HistoryRequest> takeHistoryRequests();
    void applyHistory(const std::vector<core::StableId>& entries,
                      const std::vector<std::array<std::int64_t, 6>>& history);
    void historyUnavailable(const std::vector<core::StableId>& entries);
    void forgetHistory(const std::vector<std::string>& raw_paths);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  signals:
    // The groups of an expression grouping became known, or stopped being.
    void expressionGroupsChanged();
    // Rows of a view that reads listening history have none yet.
    void historyWanted();
    void historyChanged();
    void historyRowsRestored(const QList<int>& rows);
    void historyDiscarded(const QString& reason);

  private:
    friend class BenchMainWindowTest;
    std::unordered_set<std::string> missing_;
    struct ListeningCell {
        bool loaded{false};
        std::optional<persistence::LocalListeningHistory> history;
        QString error;
    };
    [[nodiscard]] QVariant listeningHistoryData(const QModelIndex& index, int role) const;
    void dispatchListeningHistory();
    QPointer<ui::ListPersistenceService> listening_service_;
    mutable QCache<int, ListeningCell> listening_cache_{512};
    mutable QSet<int> listening_pending_;
    mutable bool listening_overflow_{false};
    QTimer* listening_timer_{nullptr};
    std::uint64_t listening_generation_{0};
    bool listening_busy_{false};
    struct Edit {
        enum class Kind : std::uint8_t { reorder, removal, addition, replacement };
        Kind kind{Kind::reorder};
        QString label;
        std::vector<int> positions;
        std::vector<LocalTrackRow> detached;
        // Current row order -> previous row order; inverted after each replay.
        std::vector<int> order;
    };
    void rememberEdit(Edit edit);
    void trimHistory();
    void replayEdit(Edit& edit, bool undo);
    void removePositions(const std::vector<int>& positions, std::vector<LocalTrackRow>* detached);
    void applyOrder(const std::vector<int>& order);
    [[nodiscard]] std::vector<LocalTrackRow*> retainedRows();
    void touched() {
        ++content_revision_;
        scheduleEvaluation();
    }
    // ADR-0282, in local_list_view_state.cpp.
    struct PendingEvaluation {
        core::StableId entry;
        std::uint64_t ticket{0U};
        int hint{-1};
    };
    struct GroupHeaderCache {
        QString title;
        QString details;
    };
    void forgetEvaluation(LocalTrackRow& row);
    void scheduleEvaluation();
    void evaluatePending();
    void mergeEvaluation(std::uint64_t view, const std::vector<PendingEvaluation>& pending,
                         std::vector<lists::EvaluatedRow> evaluated);
    void regroup();
    void mergeHeaders(std::uint64_t view, const std::vector<QString>& keys,
                      std::vector<lists::GroupHeader> headers);
    [[nodiscard]] bool evaluatedNow(const LocalTrackRow& row) const noexcept {
        return row.evaluated != nullptr && row.evaluated_view == view_generation_;
    }
    [[nodiscard]] QVariant viewData(const QModelIndex& index, int role) const;
    [[nodiscard]] QVariant viewStyle(const QModelIndex& index, int role) const;
    std::shared_ptr<const lists::CompiledListView> list_view_;
    std::uint64_t view_generation_{0U};
    int expression_columns_{0};
    QTimer* evaluation_timer_{nullptr};
    bool evaluating_{false};
    bool headers_evaluating_{false};
    bool groups_ready_{false};
    // Per row once grouped: where a group starts, its header's cache key,
    // and where a subgroup starts, its label.
    std::vector<std::uint8_t> group_starts_;
    std::vector<QString> group_header_keys_;
    std::vector<QString> subgroup_labels_;
    QHash<QString, GroupHeaderCache> group_headers_;
    std::uint64_t content_revision_{0};
    std::vector<Edit> history_;
    std::size_t history_cursor_{0};
    void refreshCurrentRow();
    void emitRowChanged(int row);
    void emitCurrentRowChanged(int row);

    std::vector<LocalTrackRow> rows_;
    QHash<QString, QImage> artwork_;
    LocalTrackSource current_source_;
    int current_row_{-1};
};

} // namespace trackknife::bench
