// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bench/musicbrainz_lookup.hpp"
#include "trackknife/musicbrainz/album_lookup.hpp"

#include <QObject>
#include <QString>

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace trackknife::bench {

// ADR-0261: albums looked up in turn, one request at a time, at MusicBrainz's
// pace -- the engine paces and caches; asking one thing at a time keeps a
// batch of thousands from waiting as thousands of threads. An album that
// carries a release id is looked up by it, without a search; one whose id
// MusicBrainz does not know, and every other, by a search and the few
// releases of it worth a look.
class AlbumLookupQueue final : public QObject {
    Q_OBJECT
  public:
    explicit AlbumLookupQueue(MusicBrainzLookupService service, QObject* parent = nullptr);

    // Looked up after those added before; `id` is the caller's.
    void add(std::size_t id, musicbrainz::AlbumQuery query);
    // Nothing more is asked; an answer on its way is dropped. Albums added
    // afterwards are looked up again.
    void stop();
    // `id` is not looked up, or no longer: its answer, if asked, is dropped.
    void forget(std::size_t id);
    [[nodiscard]] bool running() const { return current_.has_value(); }
    [[nodiscard]] std::size_t waiting() const { return waiting_.size(); }
    // Requests a batch still has to make, at most: for the time left.
    [[nodiscard]] std::size_t requestsLeft() const;

    // At most this many releases of a search are looked up for an album.
    static constexpr std::size_t releases_per_search = 3U;

  signals:
    void looking(std::size_t id);
    void lookedUp(std::size_t id, const musicbrainz::AlbumLookupResult& result);
    void failed(std::size_t id, const QString& why);
    // Nothing waiting and nothing being asked.
    void idle();

  private:
    struct Album {
        std::size_t id{0U};
        musicbrainz::AlbumQuery query;
        // The releases still to look up, and those looked up.
        std::vector<std::string> to_examine;
        std::vector<musicbrainz::Release> examined;
    };
    void next();
    void lookUpRelease(const std::string& id, bool fall_back_to_search);
    void search();
    void examineNext();
    void finish();
    void fail(const QString& why);
    void fetch(const std::string& url, std::function<void(core::Result<QByteArray>)> then);

    MusicBrainzLookupService service_;
    std::deque<Album> waiting_;
    std::optional<Album> current_;
    // Bumped by stop(): an answer for an older generation is dropped.
    quint64 generation_{0};
};

} // namespace trackknife::bench
