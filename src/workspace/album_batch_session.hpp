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
    };

    AlbumBatchSession(TaggerSession& tagger, MusicBrainzLookupService service,
                      QObject* parent = nullptr);
    ~AlbumBatchSession() override;

    [[nodiscard]] const std::vector<Album>& albums() const { return albums_; }
    [[nodiscard]] std::size_t fileCount() const;
    [[nodiscard]] std::size_t count(State state) const;
    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] bool lookingUp() const;
    [[nodiscard]] std::size_t requestsLeft() const;

    // Step 1, before anything is looked up.
    // One album per folder it spans.
    void splitByFolder(std::size_t album);
    // `album` joins `into`; the album list keeps `into`'s place.
    void merge(std::size_t album, std::size_t into);
    void setIncluded(std::size_t album, bool included);

    // Step 2: every album not left out, in turn.
    void lookUp();
    void stop();

    // Step 3, review.
    // The first album after `after` (from the start without one) that needs
    // a person, wrapping round; none when none does.
    [[nodiscard]] std::optional<std::size_t> nextNeedingYou(std::optional<std::size_t> after) const;
    // What the matcher pairs for an album: its files, as descriptors and
    // paths, and the tagger's items they are.
    struct Files {
        std::vector<musicbrainz::LocalTrackDescriptor> descriptors;
        std::vector<QString> paths;
        std::vector<std::size_t> items;
    };
    [[nodiscard]] Files filesOf(std::size_t album) const;
    // The pairing the person confirmed, staged as a clear match would be.
    void choose(std::size_t album, std::size_t version, metadata::MetadataProposalSet proposals);
    void skip(std::size_t album);

    // Step 4, Apply: the albums chosen of those staged written, the rest
    // staying staged; one write at a time, once nothing is being staged.
    [[nodiscard]] bool canWrite() const;
    // The fields each album's staged draft changes, over all its files.
    [[nodiscard]] std::vector<std::size_t> changes() const;
    void write(std::vector<std::size_t> albums, AlbumBatchWrite::Options options);
    [[nodiscard]] const AlbumBatchWrite* writing() const { return writer_; }
    void stopWriting();

    static QString stateText(const Album& album);

  signals:
    void changed();
    // The write is over, its outcome in each album's state and note.
    void writeFinished();

  private:
    void group();
    void describe(Album& album) const;
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
    QPointer<AlbumBatchWrite> writer_;
};

} // namespace trackknife::bench
