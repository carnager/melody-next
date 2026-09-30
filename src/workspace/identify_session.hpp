// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {

// Pairing the selected files with a release's tracks (ADR-0090): suggested
// by similarity off the UI thread, then changed by moving files up and
// down, leaving one unmatched, or ordering them by filename; the pairs are
// staged as ADR-0086 proposals. MusicBrainz tracks stay in album order.
class TrackMatchSession final : public QObject {
    Q_OBJECT

  public:
    // A row: a local file (or a gap) beside the album track at the same row.
    struct Row {
        QString file;
        QString file_tool_tip;
        QString length;
        QString pairing;
        QString pairing_tool_tip;
        bool paired{false};
        QString track;
        QString track_length;
    };

    TrackMatchSession(musicbrainz::Release release,
                      std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                      std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
                      QObject* parent = nullptr);
    ~TrackMatchSession() override;

    [[nodiscard]] QString heading() const;
    [[nodiscard]] static QString help();
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::vector<Row>& rows() const { return rows_; }
    [[nodiscard]] bool canMoveUp(int row) const;
    [[nodiscard]] bool canMoveDown(int row) const;
    [[nodiscard]] bool canUnmatch(int row) const;
    [[nodiscard]] bool canStage() const;

    // A file moved from one row to another (a drag), or up or down by one.
    void moveFile(std::size_t from, std::size_t to);
    void move(int row, int direction);
    // Below the album's tracks, leaving a gap; it gets no tags.
    void unmatch(int row);
    // Paired in natural filename order, or the selection's own.
    void resetOrder(bool by_filename);
    void stage();

  signals:
    // The rows changed; `select` is the row to make current, or -1.
    void changed(int select);
    void accepted(metadata::MetadataProposalSet proposals);

  private:
    [[nodiscard]] QString localPath(std::size_t local) const;
    [[nodiscard]] QString trackLabel(std::size_t target) const;
    void refresh(std::optional<std::size_t> selected = std::nullopt);

    musicbrainz::Release release_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<QString> local_paths_;
    std::vector<std::size_t> item_indexes_;
    musicbrainz::ReleaseAlignment alignment_;
    std::vector<std::optional<std::size_t>> assignments_;
    std::vector<std::optional<std::size_t>> slots_;
    std::vector<Row> rows_;
    QString status_;
    bool ready_{false};
    core::CancellationSource cancellation_;
};

// Identifying the selected files with MusicBrainz (ADR-0090, ADR-0096): a
// text search needing no MusicBrainz tags, or audio fingerprints through
// AcoustID, each release version its own candidate; the chosen one opens a
// TrackMatchSession. It never writes anything itself.
class IdentifySession final : public QObject {
    Q_OBJECT

  public:
    struct Candidate {
        QString match;
        QString match_tool_tip;
        QString album;
        QString album_tool_tip;
        QString artist;
        QString tracks;
        QString media;
        QString version;
    };

    IdentifySession(MusicBrainzLookupService service,
                    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                    std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
                    QString initial_artist, QString initial_release, QObject* parent = nullptr);
    ~IdentifySession() override;

    [[nodiscard]] QString initialArtist() const { return initial_artist_; }
    [[nodiscard]] QString initialRelease() const { return initial_release_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool canSearch() const { return !busy_; }
    [[nodiscard]] bool canScan() const;
    [[nodiscard]] const std::vector<Candidate>& candidates() const { return rows_; }
    // The row to start on once there are results; -1 for none.
    [[nodiscard]] int suggested() const { return suggested_; }
    [[nodiscard]] TrackMatchSession* match() const { return match_; }

    void search(const QString& artist, const QString& release);
    void scan();
    // The candidate at `row`, its track list loaded to be matched.
    void use(int row);
    // From matching back to the candidates.
    void back();

  signals:
    void changed();
    void matchOpened(trackknife::bench::TrackMatchSession* match);
    void matchClosed();
    void accepted(metadata::MetadataProposalSet proposals);

  private:
    void setBusy(bool busy);
    void scanFile(std::size_t position);
    void finishScan();
    void loadScanCandidate(std::size_t rank);
    void finishSearch(core::Result<QByteArray> body);
    void finishLookup(core::Result<QByteArray> body);

    MusicBrainzLookupService service_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<QString> local_paths_;
    std::vector<std::size_t> item_indexes_;
    QString initial_artist_;
    QString initial_release_;
    std::vector<musicbrainz::Release> candidates_;
    // Parallel to rows_: the candidate each row is.
    std::vector<std::size_t> row_candidates_;
    std::vector<Candidate> rows_;
    int suggested_{-1};
    QString status_;
    bool busy_{false};
    std::map<std::string, std::set<std::size_t>> scan_votes_;
    std::vector<std::pair<std::string, std::size_t>> scan_candidates_;
    std::size_t scan_matched_files_{0U};
    QPointer<TrackMatchSession> match_;
};

} // namespace trackknife::bench
