// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/track_source.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/persistence/list_repository.hpp"
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

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
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
    // True once a probe ran or persisted metadata was restored; unprobed rows
    // fall back to their file name and are queued for enrichment.
    bool probed{false};

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
    [[nodiscard]] bool hasArtwork(const QString& key) const { return artwork_.contains(key); }
    void setArtwork(const QString& key, QImage image);
    [[nodiscard]] const std::vector<LocalTrackRow>& rows() const noexcept { return rows_; }
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

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  signals:
    void historyChanged();
    void historyRowsRestored(const QList<int>& rows);
    void historyDiscarded(const QString& reason);

  private:
    friend class BenchMainWindowTest;
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
