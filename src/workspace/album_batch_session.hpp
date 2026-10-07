// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "workspace/album_batch_write.hpp"
#include "trackknife/musicbrainz/album_groups.hpp"
#include "trackknife/metadata/proposal.hpp"
#include "trackknife/musicbrainz/album_lookup.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace trackknife::bench {

class AlbumLookupQueue;
class TaggerSession;

// ADR-0261: Identify albums… over the files open in a tagger -- grouped into
// albums, looked up in the background, and each clear match staged into the
// tagger's draft by itself, one after another, as the person works.
class AlbumBatchSession final : public QObject {
    Q_OBJECT
  public:
    enum class State : std::uint8_t {
        waiting,
        searching,
        // Matched clearly, waiting for the tagger to take it.
        staging,
        staged,
        needs_choice,
        no_match,
        failed,
        left_out,
        // Passed over in review: stays for later.
        skipped,
        // Its staged tags are in its files.
        written,
        // Split into albums of its own folders, or merged into another:
        // gone from the list, kept so that no album's place changes.
        replaced,
        merged,
    };
    struct Album {
        musicbrainz::AlbumGroupBasis basis{musicbrainz::AlbumGroupBasis::folder};
        // The tagger's items, by disc and track.
        std::vector<std::size_t> items;
        std::vector<std::string> folders;
        QString artist;
        QString title;
        QString year;
        State state{State::waiting};
        // Why it failed, or could not be staged.
        QString note;
        std::optional<musicbrainz::AlbumLookupResult> result;
        // The version of result's candidates staged: the best, unless the
        // person chose another.
        std::size_t version{0U};
        // Staged, but not to be written by the next Write.
        bool excluded{false};
        // The pairing the person confirmed, staged again after an undo.
        std::optional<metadata::MetadataProposalSet> chosen{};
    };

    AlbumBatchSession(TaggerSession& tagger, MusicBrainzLookupService service,
                      QObject* parent = nullptr);
    ~AlbumBatchSession() override;

    [[nodiscard]] const std::vector<Album>& albums() const { return albums_; }
    // The albums shown, in their order: an album's place in albums() never
    // changes, so split and merge can happen while albums are looked up.
    [[nodiscard]] const std::vector<std::size_t>& order() const { return order_; }
    // Not staged nor written: its grouping can still change.
    [[nodiscard]] bool editable(std::size_t album) const;
    // Staged, and its files already say so: the release staged is what they
    // are tagged as -- an album written before, opened again. The Write's
    // own test: no tag of its files changes. Asked of the tagger's draft as
    // it is now, so an edit made since counts.
    [[nodiscard]] bool alreadyTagged(std::size_t album) const;

    [[nodiscard]] std::size_t fileCount() const;
    [[nodiscard]] std::size_t count(State state) const;
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool lookingUp() const;
    [[nodiscard]] std::size_t requestsLeft() const;

    // Grouping, on albums not yet staged; those changed are looked up again.
    // One album per folder it spans.
    void splitByFolder(std::size_t album);
    // `album` joins `into`; the album list keeps `into`'s place.
    void merge(std::size_t album, std::size_t into);
    // Left out of the lookup; a staged album, out of the next Write.
    void setIncluded(std::size_t album, bool included);

    // Step 2: every album not left out, in turn.
    void lookUp();
    void stop();

    // Step 3, review.
    // The first album after `after` (from the start without one) that needs
    // a person, wrapping round; none when none does.
    [[nodiscard]] std::optional<std::size_t> nextNeedingYou(std::optional<std::size_t> after) const;
    // What the matcher pairs for an album: its files, as descriptors and
    // paths, and the tagger's items they are -- then, ADR-0265, those of
    // the other albums whose grouping can still change, offered, each with
    // the album it is in (`other_albums`, one per offered file, last).
    struct Files {
        std::vector<musicbrainz::LocalTrackDescriptor> descriptors;
        std::vector<QString> paths;
        std::vector<std::size_t> items;
        std::vector<QString> other_albums;
    };
    [[nodiscard]] Files filesOf(std::size_t album) const;
    // The pairing the person confirmed, staged as a clear match would be.
    // A file of another album in it joins this one; the album it leaves is
    // looked up again, or goes when it is left with none.
    void choose(std::size_t album, std::size_t version, metadata::MetadataProposalSet proposals);
    void skip(std::size_t album);

    // Write (ADR-0262): the staged albums not excluded, written as the
    // tagger's Actions say -- renamed, moved, ReplayGain measured first --
    // the rest staying staged; once nothing is being staged.
    [[nodiscard]] bool canWrite() const;
    [[nodiscard]] std::vector<std::size_t> toWrite() const;
    void write();
    [[nodiscard]] const AlbumBatchWrite* writing() const { return writer_; }
    // Measuring ReplayGain before the write.
    [[nodiscard]] bool measuring() const { return measuring_; }
    // How the last Write went, said once it is over.
    [[nodiscard]] QString writeSummary() const { return write_summary_; }
    // ADR-0263: the last Write undone -- every file it wrote put back as it
    // was -- and the albums it wrote staged again, as before it.
    [[nodiscard]] bool canUndoLastWrite() const;
    void undoLastWrite();
    [[nodiscard]] bool undoing() const { return undoing_; }
    void stopWriting();

    static QString stateText(const Album& album);

  signals:
    void changed();
    // The write is over, its outcome in each album's state and note.
    void writeFinished();
    // The undo is over, said in writeSummary().
    void undoFinished();

  private:
    // Looked at whenever the tagger's draft changes: changed() when what is
    // already tagged so is not what it was.
    void noteDraft();
    void group();
    void describe(Album& album) const;
    // Its folders, from its files.
    void refolder(Album& album) const;
    void take(std::size_t album, std::size_t item);
    [[nodiscard]] musicbrainz::AlbumQuery queryOf(const Album& album) const;
    void lookedUp(std::size_t album, const musicbrainz::AlbumLookupResult& result);
    void stageNext();
    void settled(bool staged);

    QPointer<TaggerSession> tagger_;
    MusicBrainzLookupService service_;
    AlbumLookupQueue* queue_;
    std::vector<Album> albums_;
    bool started_{false};
    // Albums waiting for the tagger -- with the person's pairing, or the
    // best candidate's -- and the one it is staging.
    struct ToStage {
        std::size_t album{0U};
        std::optional<metadata::MetadataProposalSet> proposals;
    };
    std::deque<ToStage> to_stage_;
    std::optional<std::size_t> staging_;
    void startWriter(std::vector<std::size_t> albums);
    void queueLookUp(std::size_t album);

    std::vector<std::size_t> order_;
    QPointer<AlbumBatchWrite> writer_;
    bool measuring_{false};
    QString write_summary_;
    // What the last Write wrote, newest first, and the albums it wrote.
    struct LastWrite {
        std::vector<operations::UndoRequest> requests;
        std::vector<std::size_t> albums;
    };
    std::optional<LastWrite> last_write_;
    bool undoing_{false};
    void undone(std::shared_ptr<core::Result<std::vector<operations::UndoOutcome>>> outcome);
    // The albums already tagged so, as last told.
    std::vector<std::size_t> already_tagged_;
};

} // namespace trackknife::bench
