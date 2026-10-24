// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "workspace/tagger_session.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <deque>
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
//
// ADR-0265: the files of the selection's other albums may be offered too,
// listed after the album's own and never placed by the suggestion; one put
// on a track is staged with the album, and leaves the album it was in.
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
        // A file of another album, offered and not placed.
        bool other{false};
        // A gap: a track without a file, which a file dropped on fills.
        bool gap{false};
        QString track;
        QString track_length;
    };

    // `other_albums`: for the last files, one each, the album each is in --
    // offered, not the album's own.
    TrackMatchSession(musicbrainz::Release release,
                      std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                      std::vector<QString> local_paths, std::vector<std::size_t> item_indexes,
                      QObject* parent = nullptr, std::vector<QString> other_albums = {});
    ~TrackMatchSession() override;

    [[nodiscard]] QString heading() const;
    [[nodiscard]] static QString help();
    [[nodiscard]] QString status() const { return status_; }
    // How the pairing stands: files paired, files, the album's tracks.
    [[nodiscard]] std::size_t pairedCount() const { return paired_count_; }
    [[nodiscard]] std::size_t fileCount() const { return own_count_; }
    [[nodiscard]] std::size_t trackCount() const { return alignment_.release_tracks.size(); }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] const std::vector<Row>& rows() const { return rows_; }
    [[nodiscard]] bool canMoveUp(int row) const;
    [[nodiscard]] bool canMoveDown(int row) const;
    [[nodiscard]] bool canUnmatch(int row) const;
    [[nodiscard]] bool canStage() const;

    // A file moved from one row to another (a drag), or up or down by one.
    void moveFile(std::size_t from, std::size_t to);
    // A file dropped on a gap: put in it, the rows around keeping their
    // places; where it was is left a gap, or goes when below the tracks.
    void fillGap(std::size_t from, std::size_t gap);
    [[nodiscard]] bool isGap(std::size_t row) const;
    // A file put on the album track at `track`: in its gap, or swapped with
    // the file there.
    void placeOn(std::size_t from, std::size_t track);
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
    std::vector<QString> other_albums_;
    // The album's own files come first, then those offered.
    std::size_t own_count_{0U};
    musicbrainz::ReleaseAlignment alignment_;
    std::vector<std::optional<std::size_t>> assignments_;
    std::vector<std::optional<std::size_t>> slots_;
    std::vector<Row> rows_;
    QString status_;
    std::size_t paired_count_{0U};
    bool ready_{false};
    core::CancellationSource cancellation_;
};

// Identifying the selected files with MusicBrainz (ADR-0090, ADR-0096): a
// text search needing no MusicBrainz tags, or audio fingerprints through
// AcoustID, each release version its own candidate; the chosen one opens a
// TrackMatchSession. It never writes anything itself.
//
// ADR-0310: a release named by its link or id is looked up rather than
// searched for -- typed where the search is, or the one the files are
// tagged with, looked up by start() -- with its album's other versions
// beneath it; a release group's link lists them all.
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
                    QString initial_artist, QString initial_release,
                    QString tagged_release = {}, QObject* parent = nullptr);
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

    // The release the files are tagged with, looked up, where there is one.
    void start();
    // Searched for -- or, where either names a release by its link or id,
    // that release looked up.
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
    // What named the release or album looked up.
    enum class Named : std::uint8_t { tagged, release, group };
    void lookUp(const std::string& release_id, bool tagged);
    void finishReleaseLookup(core::Result<QByteArray> body, bool tagged);
    // A release group's releases: every version of the album, after the
    // release named where one was.
    void lookUpGroup(const std::string& group_id, Named named);
    void finishGroup(core::Result<QByteArray> body, Named named);

    MusicBrainzLookupService service_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<QString> local_paths_;
    std::vector<std::size_t> item_indexes_;
    QString initial_artist_;
    QString initial_release_;
    QString tagged_release_;
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

// ADR-0311: Identify over the albums a tagger's files hold, one at a time,
// each its own IdentifySession -- its own search, versions and matching.
// Those ahead of the one shown are looked up in the background, one at a
// time, from their tagged release or their tags, so each is there when it
// is reached. The pairing confirmed for an album is its choice, or it is
// skipped; apply() stages every album chosen into the tagger's draft, one
// after another -- written with the tagger's own Write. A single album's
// choice is applied at once.
class MultiIdentifySession final : public QObject {
    Q_OBJECT

  public:
    enum class State : std::uint8_t {
        waiting,
        looking_up,
        found,
        nothing_found,
        // Its pairing confirmed, to be applied.
        chosen,
        skipped,
        // Applied, waiting for the tagger to take it.
        staging,
        staged,
    };

    MultiIdentifySession(TaggerSession& tagger, MusicBrainzLookupService service,
                         std::vector<TaggerSession::Identify> albums, QObject* parent = nullptr);
    ~MultiIdentifySession() override;

    [[nodiscard]] std::size_t count() const { return albums_.size(); }
    [[nodiscard]] IdentifySession* album(std::size_t index) const;
    // "Artist — Album", as its tags say, else its first file's name.
    [[nodiscard]] QString name(std::size_t index) const;
    [[nodiscard]] State state(std::size_t index) const;
    [[nodiscard]] QString stateText(std::size_t index) const;
    [[nodiscard]] std::size_t current() const { return current_; }
    // The album shown: looked up at once if it has not been.
    void setCurrent(std::size_t index);
    // The next album after `after`, wrapping round, neither chosen,
    // skipped nor applied.
    [[nodiscard]] std::optional<std::size_t> nextToDo(std::size_t after) const;
    // Passed over: left out of apply() until a version is chosen for it.
    void skip(std::size_t index);
    // The albums chosen and not yet applied.
    [[nodiscard]] std::size_t chosenCount() const;
    // Every album chosen staged into the tagger's draft, one after another;
    // finished() once all are.
    void apply();
    // Applied albums not yet taken by the tagger.
    [[nodiscard]] bool staging() const { return staging_.has_value() || !to_stage_.empty(); }
    // Goes once nothing it was given is left to stage: the window that
    // showed it may close before.
    void release();

  signals:
    void changed();
    // Every album applied is staged.
    void finished();

  private:
    struct Album {
        TaggerSession::Identify files;
        QPointer<IdentifySession> session;
        bool started{false};
        std::optional<metadata::MetadataProposalSet> chosen;
        bool skipped{false};
        bool applied{false};
        bool staged{false};
        QString note;
    };
    void lookUp(std::size_t index);
    void lookAhead();
    void accepted(std::size_t index, metadata::MetadataProposalSet proposals);
    void stageNext();
    void settled(bool staged);

    QPointer<TaggerSession> tagger_;
    std::vector<Album> albums_;
    std::size_t current_{0U};
    std::deque<std::pair<std::size_t, metadata::MetadataProposalSet>> to_stage_;
    std::optional<std::size_t> staging_;
    // The tagger refused one of the albums applied last: said, not closed.
    bool refused_{false};
    bool released_{false};
};

} // namespace trackknife::bench
