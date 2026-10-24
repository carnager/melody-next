// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/acoustid.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using namespace trackknife::musicbrainz;

void searchUrlEscapesLuceneAndPercentEncodes() {
    const auto url = build_release_search_url(ReleaseSearchQuery{
        .artist = "AC/DC \"Live\"",
        .release = "Back in Black",
        .track_count = 10U,
        .limit = 5U,
    });
    CHECK(url.has_value());
    CHECK(url->starts_with("https://musicbrainz.org/ws/2/release/?query="));
    CHECK(url->ends_with("&fmt=json&limit=5"));
    // The quote inside the artist phrase is Lucene-escaped, then everything
    // is percent-encoded: \" becomes %5C%22.
    CHECK(url->find("%5C%22Live%5C%22") != std::string::npos);
    CHECK(url->find("tracks%3A10") != std::string::npos);
    CHECK(url->find(' ') == std::string::npos);

    CHECK(!build_release_search_url(ReleaseSearchQuery{}).has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 0U})
               .has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 999U})
               .has_value());
}

void lookupUrlRequiresAReleaseId() {
    const auto url = build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://musicbrainz.org/ws/2/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc"
                  "?inc=artist-credits+recordings+release-groups+labels"
                  "+isrcs+recording-level-rels+work-rels&fmt=json");
    CHECK(!build_release_lookup_url("").has_value());
    CHECK(!build_release_lookup_url("not-a-uuid").has_value());
    CHECK(!build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789ab/../x").has_value());
}

// A release named by its id or its link; nothing else, a release group's
// link included.
void releaseIdsAreFoundInLinks() {
    constexpr std::string_view id = "2f2ac1b7-1111-4f4f-8f8f-123456789abc";
    CHECK(release_id_in(id) == std::string{id});
    CHECK(release_id_in("  2F2AC1B7-1111-4F4F-8F8F-123456789ABC\t") == std::string{id});
    CHECK(release_id_in("https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc") ==
          std::string{id});
    CHECK(release_id_in("musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/discids") ==
          std::string{id});
    CHECK(release_id_in("https://beta.musicbrainz.org/release/"
                        "2f2ac1b7-1111-4f4f-8f8f-123456789abc?tport=8000") == std::string{id});
    CHECK(!release_id_in("").has_value());
    CHECK(!release_id_in("Alpha").has_value());
    CHECK(!release_id_in("https://musicbrainz.org/release-group/"
                         "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
    CHECK(!release_id_in("https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abcd")
               .has_value());
    CHECK(!release_id_in("https://notmusicbrainz.org/release/"
                         "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
    // An album's link names its release group; a bare id is a release's.
    CHECK(release_group_id_in("https://musicbrainz.org/release-group/"
                              "2F2AC1B7-1111-4f4f-8f8f-123456789abc") == std::string{id});
    CHECK(!release_group_id_in(id).has_value());
    CHECK(!release_group_id_in("https://musicbrainz.org/release/"
                               "2f2ac1b7-1111-4f4f-8f8f-123456789abc")
               .has_value());
}

// Every version of an album: a release group's releases, read as a search's.
void releaseGroupReleasesAreListed() {
    const auto url = build_release_group_releases_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://musicbrainz.org/ws/2/release?release-group="
                  "2f2ac1b7-1111-4f4f-8f8f-123456789abc"
                  "&inc=artist-credits+labels+media+release-groups&fmt=json&limit=100");
    CHECK(!build_release_group_releases_url("not-a-uuid").has_value());
    const auto listed = parse_release_search(R"json({"release-count": 140, "releases": [
        {"id": "2f2ac1b7-1111-4f4f-8f8f-123456789abc", "title": "Alpha",
         "media": [{"format": "CD", "track-count": 2}, {"format": "CD", "track-count": 3}]}]})json");
    CHECK(listed.has_value());
    CHECK(listed->total_count == 140U);
    CHECK(listed->releases.size() == 1U);
    CHECK(listed->releases.front().track_count == 5U);
}

constexpr std::string_view search_fixture = R"json({
  "count": 42,
  "offset": 0,
  "releases": [
    {
      "id": "11111111-2222-3333-4444-555555555555",
      "score": 100,
      "title": "Alpha",
      "status": "Official",
      "disambiguation": "remastered",
      "date": "1999-09-09",
      "country": "DE",
      "barcode": "0123456789012",
      "track-count": 10,
      "artist-credit": [
        {"name": "Band", "joinphrase": " feat. ",
         "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                    "name": "Band", "sort-name": "Band, The"}},
        {"name": "Guest", "artist": {"id": "ffffffff-0000-1111-2222-333333333333",
                                     "name": "Guest", "sort-name": "Guest"}}
      ],
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "label-info": [
        {"catalog-number": "CAT-123", "label": {"name": "Label Records"}}
      ],
      "media": [{"format": "CD", "track-count": 10}]
    },
    {
      "id": "21111111-2222-3333-4444-555555555555",
      "score": 87,
      "title": "Alpha",
      "status": "Official",
      "date": "2001-01-01",
      "country": "US",
      "track-count": 11,
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "media": [{"format": "12\" Vinyl", "track-count": 11}]
    }
  ]
})json";

void searchParsingCarriesVersionDetail() {
    const auto result = parse_release_search(search_fixture);
    CHECK(result.has_value());
    if (!result) {
        return;
    }
    CHECK(result->total_count == 42U);
    CHECK(result->releases.size() == 2U);
    const auto& first = result->releases.front();
    CHECK(first.id == "11111111-2222-3333-4444-555555555555");
    CHECK(first.title == "Alpha");
    CHECK(first.search_score == 100);
    CHECK(first.disambiguation == "remastered");
    CHECK(first.date == "1999-09-09");
    CHECK(first.country == "DE");
    CHECK(first.barcode == "0123456789012");
    CHECK(first.release_group_id == "99999999-8888-7777-6666-555555555555");
    CHECK(first.label == "Label Records");
    CHECK(first.catalog_number == "CAT-123");
    CHECK(first.track_count == 10U);
    CHECK(first.artist_credits.size() == 2U);
    CHECK(first.artist_credits[0].name == "Band");
    CHECK(first.artist_credits[0].join_phrase == " feat. ");
    CHECK(first.artist_credits[0].artist_id == "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    CHECK(first.artist_credits[0].sort_name == "Band, The");
    CHECK(first.media.size() == 1U && first.media.front().format == "CD");
    // Two versions of the same release group stay distinct candidates.
    const auto& second = result->releases.back();
    CHECK(second.release_group_id == first.release_group_id);
    CHECK(second.country == "US" && second.media.front().format == "12\" Vinyl");
}

constexpr std::string_view lookup_fixture = R"json({
  "id": "11111111-2222-3333-4444-555555555555",
  "title": "Alpha",
  "status": "Official",
  "date": "1999-09-09",
  "country": "DE",
  "release-group": {"id": "99999999-8888-7777-6666-555555555555",
                    "primary-type": "Album", "secondary-types": ["Live"],
                    "first-release-date": "1990-05-01"},
  "text-representation": {"script": "Latn", "language": "eng"},
  "artist-credit": [{"name": "Band",
                     "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                                "name": "Band", "sort-name": "Band, The"}}],
  "media": [
    {
      "position": 1,
      "format": "CD",
      "title": "First Night",
      "track-count": 2,
      "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One",
                       "isrcs": ["DEA119900001"],
                       "relations": [
                         {"type": "performance",
                          "work": {"id": "cccc1111-0000-0000-0000-000000000001",
                                   "title": "One (work)"}},
                         {"type": "performance", "work": {"id": "not-a-uuid"}}
                       ]}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002",
                       "title": "Two (recording)", "length": 59000}}
      ]
    }
  ]
})json";

void lookupParsingAlignsTracksAndRecordings() {
    const auto release = parse_release_lookup(lookup_fixture);
    CHECK(release.has_value());
    if (!release) {
        return;
    }
    CHECK(release->media.size() == 1U);
    const auto& medium = release->media.front();
    CHECK(medium.position == 1U && medium.format == "CD");
    CHECK(medium.tracks.size() == 2U);
    CHECK(medium.tracks[0].position == 1U && medium.tracks[0].title == "One");
    CHECK(medium.tracks[0].length_ms == std::optional<std::int64_t>{61'000});
    CHECK(medium.tracks[0].recording_id == "bbbb1111-0000-0000-0000-000000000001");
    // Track title/length fall back to the recording when absent.
    CHECK(medium.tracks[1].title == "Two (recording)");
    CHECK(medium.tracks[1].length_ms == std::optional<std::int64_t>{59'000});
    CHECK(release->track_count == 2U);

    // Picard-parity detail: release-group dates and types, the text
    // representation, medium titles, ISRCs, and valid work ids only.
    CHECK(release->release_group_primary_type == "Album");
    CHECK(release->release_group_secondary_types == std::vector<std::string>{"Live"});
    CHECK(release->release_group_first_release_date == "1990-05-01");
    CHECK(release->script == "Latn");
    CHECK(release->language == "eng");
    CHECK(medium.title == "First Night");
    CHECK(medium.tracks[0].isrcs == std::vector<std::string>{"DEA119900001"});
    CHECK(medium.tracks[0].work_ids ==
          std::vector<std::string>{"cccc1111-0000-0000-0000-000000000001"});
    CHECK(medium.tracks[1].isrcs.empty());
    CHECK(medium.tracks[1].work_ids.empty());
}

void malformedAndOversizedPayloadsFailTyped() {
    CHECK(!parse_release_search("not json").has_value());
    CHECK(!parse_release_search("[1,2,3]").has_value());
    CHECK(!parse_release_search(R"({"count": 1})").has_value());
    CHECK(!parse_release_search(R"({"count": 1, "releases": [{"title": "No id"}]})").has_value());
    WebServiceLimits tiny;
    tiny.body_bytes = 8U;
    const auto oversized = parse_release_search(search_fixture, tiny);
    CHECK(!oversized.has_value() &&
          oversized.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

constexpr std::string_view cover_art_fixture = R"json({
  "images": [
    {"id": 987654321, "front": false, "back": true, "approved": true,
     "comment": "reverse", "types": ["Back"],
     "image": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/987654321.jpg"},
    {"id": "123456789", "front": true, "back": false, "approved": true,
     "comment": "", "types": ["Front"],
     "image": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789.png",
     "thumbnails": {"250": "http://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789-250.jpg",
                    "small": "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/123456789-small.jpg"}},
    {"id": 42, "front": false, "back": false, "approved": false, "types": ["Booklet"],
     "image": "ftp://nowhere.example/booklet.png"}
  ],
  "release": "https://musicbrainz.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc"
})json";

void coverArtUrlRequiresAReleaseId() {
    const auto url = build_cover_art_listing_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(!build_cover_art_listing_url("").has_value());
    CHECK(!build_cover_art_listing_url("not-a-uuid").has_value());
}

void coverArtParsingUpgradesAndSelectsTheFront() {
    const auto listing = parse_cover_art_listing(cover_art_fixture);
    CHECK(listing.has_value());
    // The ftp entry has no usable https image and is dropped.
    CHECK(listing->images.size() == 2U);
    CHECK(listing->images[0].id == "987654321");
    CHECK(listing->images[0].back);
    CHECK(listing->images[0].comment == "reverse");
    CHECK(listing->images[1].id == "123456789");
    CHECK(listing->images[1].front);
    CHECK(listing->images[1].types == std::vector<std::string>{"Front"});
    CHECK(listing->images[1].image_url ==
          "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/"
          "123456789.png");
    // The 250px thumbnail wins and is https-upgraded; the back image has
    // none.
    CHECK(listing->images[1].thumbnail_url ==
          "https://coverartarchive.org/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc/"
          "123456789-250.jpg");
    CHECK(listing->images[0].thumbnail_url.empty());
    CHECK(select_front_cover(*listing) == std::optional<std::size_t>{1U});

    // Without a flagged front image, an approved image is preferred and an
    // empty listing selects nothing.
    auto no_front = *listing;
    no_front.images[1].front = false;
    no_front.images[1].types = {"Booklet"};
    no_front.images[1].approved = false;
    CHECK(select_front_cover(no_front) == std::optional<std::size_t>{0U});
    CHECK(select_front_cover(CoverArtListing{}) == std::nullopt);

    CHECK(!parse_cover_art_listing("{}").has_value());
    WebServiceLimits tiny;
    tiny.cover_art_images = 1U;
    const auto oversized = parse_cover_art_listing(cover_art_fixture, tiny);
    CHECK(!oversized.has_value() &&
          oversized.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

constexpr std::string_view acoustid_fixture = R"json({
  "status": "ok",
  "results": [
    {"id": "aaa", "score": 0.65,
     "recordings": [{"id": "bbbb1111-0000-0000-0000-000000000002",
                     "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}]},
    {"id": "bbb", "score": 0.98,
     "recordings": [
       {"id": "bbbb1111-0000-0000-0000-000000000001",
        "releases": [{"id": "11111111-2222-3333-4444-555555555555"},
                     {"id": "99999999-8888-7777-6666-555555555555"},
                     {"id": "not-a-uuid"}]},
       {"id": "not-a-uuid", "releases": [{"id": "11111111-2222-3333-4444-555555555555"}]}
     ]}
  ]
})json";

void acoustidBodyAndParsingAreBounded() {
    const auto body = build_acoustid_lookup_body("key", 331U, "AQADtKISJUzUR/Ph+3lM+A==");
    CHECK(body.has_value());
    CHECK(body->find("client=key") != std::string::npos);
    CHECK(body->find("meta=recordings+releases") != std::string::npos);
    CHECK(body->find("duration=331") != std::string::npos);
    // The fingerprint is percent-encoded into the form body.
    CHECK(body->find("AQADtKISJUzUR%2FPh%2B3lM%2BA%3D%3D") != std::string::npos);
    CHECK(!build_acoustid_lookup_body("", 331U, "fp").has_value());
    CHECK(!build_acoustid_lookup_body("key", 0U, "fp").has_value());
    CHECK(!build_acoustid_lookup_body("key", 331U, "").has_value());

    const auto lookup = parse_acoustid_lookup(acoustid_fixture);
    CHECK(lookup.has_value());
    if (!lookup) {
        return;
    }
    // Results arrive sorted by score, invalid recording ids are dropped,
    // and release lists keep only valid UUIDs.
    CHECK(lookup->results.size() == 2U);
    CHECK(lookup->results[0].score > lookup->results[1].score);
    CHECK(lookup->results[0].recordings.size() == 1U);
    CHECK(lookup->results[0].recordings[0].id == "bbbb1111-0000-0000-0000-000000000001");
    CHECK(lookup->results[0].recordings[0].release_ids ==
          (std::vector<std::string>{"11111111-2222-3333-4444-555555555555",
                                    "99999999-8888-7777-6666-555555555555"}));

    const auto rejected = parse_acoustid_lookup(
        R"json({"status": "error", "error": {"message": "invalid API key"}})json");
    CHECK(!rejected.has_value());
    CHECK(!rejected && rejected.error().message.find("invalid API key") != std::string::npos);
}

} // namespace

int main() {
    searchUrlEscapesLuceneAndPercentEncodes();
    lookupUrlRequiresAReleaseId();
    releaseIdsAreFoundInLinks();
    releaseGroupReleasesAreListed();
    searchParsingCarriesVersionDetail();
    lookupParsingAlignsTracksAndRecordings();
    malformedAndOversizedPayloadsFailTyped();
    coverArtUrlRequiresAReleaseId();
    coverArtParsingUpgradesAndSelectsTheFront();
    acoustidBodyAndParsingAreBounded();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
