// SPDX-License-Identifier: GPL-3.0-only

// ADR-0261: albums looked up in turn, one request at a time.

#include "workspace/album_lookup_queue.hpp"

#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <map>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr auto search_body = R"json({
  "count": 1,
  "releases": [{
    "id": "11111111-2222-3333-4444-555555555555",
    "score": 100, "title": "Alpha", "status": "Official",
    "date": "1999-09-09", "country": "DE", "track-count": 2,
    "artist-credit": [{"name": "Band",
      "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
    "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
    "media": [{"format": "CD", "track-count": 2}]
  }]
})json";

constexpr auto lookup_body = R"json({
  "id": "11111111-2222-3333-4444-555555555555",
  "title": "Alpha", "status": "Official", "date": "1999-09-09", "country": "DE",
  "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
  "artist-credit": [{"name": "Band",
    "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Band"}}],
  "media": [{"position": 1, "format": "CD", "track-count": 2, "tracks": [
    {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
     "title": "One", "length": 61000,
     "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
    {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
     "title": "Two", "length": 59000,
     "recording": {"id": "bbbb1111-0000-0000-0000-000000000002", "title": "Two"}}
  ]}]
})json";

constexpr auto known_id = "11111111-2222-3333-4444-555555555555";

// MusicBrainz as the engine answers it: later, never at once.
struct FakeMusicBrainz {
    std::vector<QString> asked;
    int in_flight{0};
    int most_in_flight{0};
    bool searches_fail{false};

    [[nodiscard]] MusicBrainzLookupService service() {
        return MusicBrainzLookupService{
            .fetch =
                [this](const QString& url, std::function<void(core::Result<QByteArray>)> then) {
                    asked.push_back(url);
                    most_in_flight = std::max(most_in_flight, ++in_flight);
                    QTimer::singleShot(1, [this, url, then = std::move(then)] {
                        --in_flight;
                        if (url.contains(QStringLiteral("?query="))) {
                            if (searches_fail) {
                                then(std::unexpected(core::Error{.code = core::ErrorCode::io,
                                                                 .message = "offline",
                                                                 .context = {}}));
                                return;
                            }
                            then(QByteArray{search_body});
                            return;
                        }
                        if (url.contains(QString::fromLatin1(known_id))) {
                            then(QByteArray{lookup_body});
                            return;
                        }
                        then(std::unexpected(core::Error{.code = core::ErrorCode::not_found,
                                                         .message = "no such release",
                                                         .context = {}}));
                    });
                },
            .fingerprint = {},
            .acoustid_lookup = {},
        };
    }
    [[nodiscard]] int searches() const {
        return static_cast<int>(std::ranges::count_if(
            asked, [](const QString& url) { return url.contains(QStringLiteral("?query=")); }));
    }
};

[[nodiscard]] musicbrainz::AlbumQuery album(std::string release_id, std::string artist = "Band",
                                            std::string title = "Alpha") {
    const auto file = [](std::string name, std::size_t number, std::int64_t length) {
        return musicbrainz::LocalTrackDescriptor{.title = std::move(name),
                                                 .artist = {},
                                                 .album = {},
                                                 .track_number = number,
                                                 .disc_number = {},
                                                 .duration_ms = length};
    };
    return musicbrainz::AlbumQuery{.release_id = std::move(release_id),
                                   .artist = std::move(artist),
                                   .album = std::move(title),
                                   .tracks = {file("One", 1U, 61'500), file("Two", 2U, 58'800)}};
}

} // namespace

class AlbumLookupQueueTest : public QObject {
    Q_OBJECT
  private slots:
    void anIdIsLookedUpWithoutASearch();
    void anUnknownIdAndNoIdAreSearched();
    void oneThingAtATimeInTurn();
    void untaggedAlbumsAskNothing();
    void untaggedAlbumsAreHeard();
    void aFailureIsSaidAndTheNextGoesOn();
    void stoppingDropsWhatIsOnItsWay();
};

void AlbumLookupQueueTest::anIdIsLookedUpWithoutASearch() {
    FakeMusicBrainz musicbrainz;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    QCOMPARE(queue.requestsLeft(), 0U);
    queue.add(7U, album(known_id));
    QVERIFY(queue.running());
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(found.front().at(0).value<std::size_t>(), 7U);
    const auto result = found.front().at(1).value<musicbrainz::AlbumLookupResult>();
    QVERIFY(result.outcome == musicbrainz::AlbumLookupOutcome::matched);
    QCOMPARE(QString::fromStdString(result.candidates.front().release.id),
             QString::fromLatin1(known_id));
    QCOMPARE(musicbrainz.asked.size(), 1U);
    QCOMPARE(musicbrainz.searches(), 0);
    QTRY_VERIFY(!queue.running());
}

void AlbumLookupQueueTest::anUnknownIdAndNoIdAreSearched() {
    FakeMusicBrainz musicbrainz;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    queue.add(1U, album("00000000-0000-0000-0000-000000000000"));
    queue.add(2U, album({}));
    QTRY_COMPARE(found.count(), 2);
    for (const auto& answer : found) {
        QVERIFY(answer.at(1).value<musicbrainz::AlbumLookupResult>().outcome ==
                musicbrainz::AlbumLookupOutcome::matched);
    }
    // The unknown id, then a search and its release; a search and its release.
    QCOMPARE(musicbrainz.asked.size(), 5U);
    QCOMPARE(musicbrainz.searches(), 2);
}

void AlbumLookupQueueTest::oneThingAtATimeInTurn() {
    FakeMusicBrainz musicbrainz;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy looking{&queue, &AlbumLookupQueue::looking};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    QSignalSpy idle{&queue, &AlbumLookupQueue::idle};
    for (std::size_t id = 0; id < 4U; ++id) {
        queue.add(id, album(id % 2U == 0U ? std::string{known_id} : std::string{}));
    }
    QCOMPARE(queue.waiting(), 3U);
    QCOMPARE(queue.requestsLeft(), 2U * 1U + 2U * (1U + AlbumLookupQueue::releases_per_search));
    QTRY_COMPARE(idle.count(), 1);
    QCOMPARE(found.count(), 4);
    QCOMPARE(musicbrainz.most_in_flight, 1);
    for (int index = 0; index < 4; ++index) {
        QCOMPARE(looking.at(index).at(0).value<std::size_t>(), static_cast<std::size_t>(index));
        QCOMPARE(found.at(index).at(0).value<std::size_t>(), static_cast<std::size_t>(index));
    }
    QCOMPARE(queue.requestsLeft(), 0U);
}

void AlbumLookupQueueTest::untaggedAlbumsAskNothing() {
    FakeMusicBrainz musicbrainz;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    queue.add(3U, album({}, {}, {}));
    QTRY_COMPARE(found.count(), 1);
    QVERIFY(found.front().at(1).value<musicbrainz::AlbumLookupResult>().outcome ==
            musicbrainz::AlbumLookupOutcome::no_match);
    QVERIFY(musicbrainz.asked.empty());
}

// ADR-0261: with AcoustID set up, an album with nothing to search by is
// heard -- each file fingerprinted and looked up -- and the release its files
// are on examined; a release heard on no file is not.
void AlbumLookupQueueTest::untaggedAlbumsAreHeard() {
    FakeMusicBrainz musicbrainz;
    auto service = musicbrainz.service();
    std::vector<QString> fingerprinted;
    int acoustid_asked = 0;
    service.fingerprint =
        [&fingerprinted](const QString& path,
                         std::function<void(core::Result<AcoustIdFingerprint>)> then) {
            fingerprinted.push_back(path);
            QTimer::singleShot(1, [path, then = std::move(then)] {
                // One file cannot be heard: the others still count.
                if (path.endsWith(QStringLiteral("broken.flac"))) {
                    then(std::unexpected(core::Error{
                        .code = core::ErrorCode::io, .message = "unreadable", .context = {}}));
                    return;
                }
                then(AcoustIdFingerprint{.duration_seconds = 60U, .fingerprint = path});
            });
        };
    service.acoustid_lookup =
        [&acoustid_asked](const AcoustIdFingerprint&,
                          std::function<void(core::Result<QByteArray>)> then) {
            ++acoustid_asked;
            QTimer::singleShot(1, [then = std::move(then)] {
                then(QByteArray{R"json({"status": "ok", "results": [
                {"id": "x", "score": 0.95, "recordings": [
                  {"id": "bbbb1111-0000-0000-0000-000000000001",
                   "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]},
                {"id": "y", "score": 0.2, "recordings": [
                  {"id": "bbbb1111-0000-0000-0000-000000000009",
                   "releases": [{"id": "22222222-2222-3333-4444-555555555555"}]}]}]})json"});
            });
        };
    AlbumLookupQueue queue{service};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    auto untagged = album({}, {}, {});
    untagged.paths = {"/music/rip/01.flac", "/music/rip/broken.flac", "/music/rip/02.flac"};
    untagged.tracks.push_back(untagged.tracks.back());
    queue.add(4U, untagged);
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(fingerprinted.size(), 3U);
    QCOMPARE(acoustid_asked, 2);
    QCOMPARE(musicbrainz.searches(), 0);
    // The release heard, looked up; the one below the score, not.
    QCOMPARE(musicbrainz.asked.size(), 1U);
    QVERIFY(musicbrainz.asked.front().contains(QString::fromLatin1(known_id)));
    const auto result = found.front().at(1).value<musicbrainz::AlbumLookupResult>();
    QVERIFY(result.outcome != musicbrainz::AlbumLookupOutcome::no_match);
    QCOMPARE(result.candidates.front().release.id, std::string{known_id});
}

void AlbumLookupQueueTest::aFailureIsSaidAndTheNextGoesOn() {
    FakeMusicBrainz musicbrainz;
    musicbrainz.searches_fail = true;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy failed{&queue, &AlbumLookupQueue::failed};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    queue.add(1U, album({}));
    queue.add(2U, album(known_id));
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.front().at(0).value<std::size_t>(), 1U);
    QCOMPARE(failed.front().at(1).toString(), QStringLiteral("offline"));
    QCOMPARE(found.front().at(0).value<std::size_t>(), 2U);
}

void AlbumLookupQueueTest::stoppingDropsWhatIsOnItsWay() {
    FakeMusicBrainz musicbrainz;
    AlbumLookupQueue queue{musicbrainz.service()};
    QSignalSpy found{&queue, &AlbumLookupQueue::lookedUp};
    QSignalSpy idle{&queue, &AlbumLookupQueue::idle};
    queue.add(1U, album(known_id));
    queue.add(2U, album(known_id));
    queue.stop();
    QCOMPARE(idle.count(), 1);
    QVERIFY(!queue.running());
    QCOMPARE(queue.waiting(), 0U);
    QTest::qWait(50);
    QCOMPARE(found.count(), 0);
    // And goes on when asked again.
    queue.add(3U, album(known_id));
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(found.front().at(0).value<std::size_t>(), 3U);
}

} // namespace trackknife::bench

QTEST_GUILESS_MAIN(trackknife::bench::AlbumLookupQueueTest)
#include "album_lookup_queue_test.moc"
