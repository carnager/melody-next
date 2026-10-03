// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/album_groups.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <iostream>
#include <optional>
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

[[nodiscard]] ArtistCredit credit(std::string name, std::string join_phrase, std::string artist_id,
                                  std::string sort_name = {}) {
    return ArtistCredit{
        .name = std::move(name),
        .join_phrase = std::move(join_phrase),
        .artist_id = std::move(artist_id),
        .sort_name = std::move(sort_name),
    };
}

[[nodiscard]] ReleaseTrack track(std::string title, const std::size_t position,
                                 const std::optional<std::int64_t> length_ms = std::nullopt,
                                 std::vector<ArtistCredit> credits = {}) {
    return ReleaseTrack{
        .track_id = "aaaa0000-0000-0000-0000-00000000000" + std::to_string(position),
        .recording_id = "bbbb0000-0000-0000-0000-00000000000" + std::to_string(position),
        .position = position,
        .number = std::to_string(position),
        .title = std::move(title),
        .length_ms = length_ms,
        .artist_credits = std::move(credits),
        .isrcs = {},
        .work_ids = {},
    };
}

[[nodiscard]] Release two_disc_release() {
    Release release{
        .id = "11111111-2222-3333-4444-555555555555",
        .title = "Alpha",
        .status = "Official",
        .disambiguation = "remastered",
        .date = "1999-09-09",
        .country = "DE",
        .barcode = {},
        .release_group_id = "99999999-8888-7777-6666-555555555555",
        .release_group_primary_type = "Album",
        .release_group_secondary_types = {"Live"},
        .release_group_first_release_date = "1990-01-01",
        .script = "Latn",
        .language = "eng",
        .label = "Label Records",
        .catalog_number = "CAT-123",
        .search_score = 100,
        .track_count = 3U,
        .artist_credits = {credit("Band", " feat. ", "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                                  "Band, The"),
                           credit("Guest", {}, "ffffffff-0000-1111-2222-333333333333")},
        .media = {},
    };
    release.media.push_back(ReleaseMedium{
        .position = 1U,
        .format = "CD",
        .title = "First Night",
        .track_count = 2U,
        .tracks = {track("One", 1U, 61'000), track("Two", 2U, 59'000)},
    });
    release.media.push_back(ReleaseMedium{
        .position = 2U,
        .format = "CD",
        .title = {},
        .track_count = 1U,
        .tracks = {track("Three", 1U, 63'000)},
    });
    return release;
}

void rankingCorroboratesTrackCount() {
    ReleaseSearchResult candidates;
    Release close_score;
    close_score.id = "11111111-2222-3333-4444-555555555555";
    close_score.title = "Alpha";
    close_score.search_score = 100;
    close_score.track_count = 12U;
    Release matching_count = close_score;
    matching_count.id = "21111111-2222-3333-4444-555555555555";
    matching_count.search_score = 90;
    matching_count.track_count = 3U;
    candidates.releases = {close_score, matching_count};

    const std::vector<LocalTrackDescriptor> locals(3U);
    const auto ranked = rank_release_candidates(locals, candidates);
    CHECK(ranked.size() == 2U);
    // 90 + 15 corroboration beats a bare 100.
    CHECK(ranked.front().release_index == 1U);
    CHECK(ranked.front().score == 105);

    // Without local knowledge the MusicBrainz order stands.
    const auto uncorroborated = rank_release_candidates({}, candidates);
    CHECK(uncorroborated.front().release_index == 0U);
}

void alignsByDiscAndTrackNumberPermutation() {
    const auto release = two_disc_release();
    // Local files arrive shuffled but carry exact disc/track numbers.
    const std::vector<LocalTrackDescriptor> locals{
        LocalTrackDescriptor{.title = "Three",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 1U,
                             .disc_number = 2U,
                             .duration_ms = 63'200},
        LocalTrackDescriptor{.title = "One",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 1U,
                             .disc_number = 1U,
                             .duration_ms = 61'100},
        LocalTrackDescriptor{.title = "Two",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 2U,
                             .disc_number = 1U,
                             .duration_ms = 58'900},
    };
    const auto alignment = align_release_tracks(locals, release);
    CHECK(alignment.matched_count == 3U);
    CHECK(alignment.release_tracks.size() == 3U);
    CHECK(alignment.tracks[0].release_track_index == std::optional<std::size_t>{2U});
    CHECK(alignment.tracks[1].release_track_index == std::optional<std::size_t>{0U});
    CHECK(alignment.tracks[2].release_track_index == std::optional<std::size_t>{1U});
    CHECK(alignment.tracks[0].confidence > 0.9);
    CHECK(alignment.confidence > 0.9);
}

void alignsByOrderAndThenGreedyTitles() {
    auto release = two_disc_release();
    release.media.pop_back();
    release.track_count = 2U;

    // No numbers anywhere: counts match, order aligns.
    const std::vector<LocalTrackDescriptor> ordered{
        LocalTrackDescriptor{.title = "one",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 61'000},
        LocalTrackDescriptor{.title = "two",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 59'000},
    };
    const auto by_order = align_release_tracks(ordered, release);
    CHECK(by_order.matched_count == 2U);
    CHECK(by_order.tracks[0].release_track_index == std::optional<std::size_t>{0U});
    CHECK(by_order.tracks[1].release_track_index == std::optional<std::size_t>{1U});

    // Count mismatch: greedy title matching assigns what it can and leaves
    // the stranger unmatched instead of guessing.
    const std::vector<LocalTrackDescriptor> extra{
        LocalTrackDescriptor{.title = "Two",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 59'050},
        LocalTrackDescriptor{.title = "Completely Different",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 200'000},
        LocalTrackDescriptor{.title = "One",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 61'020},
    };
    const auto greedy = align_release_tracks(extra, release);
    CHECK(greedy.matched_count == 2U);
    CHECK(greedy.tracks[0].release_track_index == std::optional<std::size_t>{1U});
    CHECK(greedy.tracks[1].release_track_index == std::nullopt);
    CHECK(greedy.tracks[1].confidence == 0.0);
    CHECK(greedy.tracks[2].release_track_index == std::optional<std::size_t>{0U});
    CHECK(greedy.confidence < by_order.confidence);
}

void proposalsCarryTagsIdentifiersAndVersion() {
    auto release = two_disc_release();
    release.media[0].tracks[0].isrcs = {"DEA119900001"};
    release.media[0].tracks[0].work_ids = {"cccc1111-0000-0000-0000-000000000001"};
    const std::vector<LocalTrackDescriptor> locals{
        LocalTrackDescriptor{.title = "One",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 1U,
                             .disc_number = 1U,
                             .duration_ms = 61'000},
        LocalTrackDescriptor{.title = "Two",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 2U,
                             .disc_number = 1U,
                             .duration_ms = 59'000},
        LocalTrackDescriptor{.title = "Three",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 1U,
                             .disc_number = 2U,
                             .duration_ms = 63'000},
    };
    const auto alignment = align_release_tracks(locals, release);
    const std::vector<std::size_t> items{4U, 7U, 9U};
    const auto proposals = release_metadata_proposals(release, alignment, items);
    CHECK(proposals.has_value());
    if (!proposals) {
        return;
    }
    CHECK(proposals->provider_name == "MusicBrainz");
    CHECK(proposals->provider_detail.find("Alpha") != std::string::npos);
    CHECK(proposals->provider_detail.find("1999-09-09") != std::string::npos);
    CHECK(proposals->provider_detail.find("remastered") != std::string::npos);
    CHECK(proposals->items.size() == 3U);

    const auto* first = &proposals->items.front();
    CHECK(first->item_index == 4U);
    const auto field =
        [first](
            const std::string_view canonical) -> const trackknife::metadata::ProposedFieldValues* {
        for (const auto& value : first->fields) {
            if (value.canonical_field == canonical) {
                return &value;
            }
        }
        return nullptr;
    };
    CHECK(field("title") != nullptr && field("title")->values.front() == "One");
    CHECK(field("artist") != nullptr && field("artist")->values.front() == "Band feat. Guest");
    CHECK(field("album") != nullptr && field("album")->values.front() == "Alpha");
    CHECK(field("albumartist") != nullptr &&
          field("albumartist")->values.front() == "Band feat. Guest");
    CHECK(field("date") != nullptr && field("date")->values.front() == "1999-09-09");
    CHECK(field("tracknumber") != nullptr && field("tracknumber")->values.front() == "1");
    // Per-medium totals plus disc numbering for the multi-disc release.
    CHECK(field("totaltracks") != nullptr && field("totaltracks")->values.front() == "2");
    CHECK(field("discnumber") != nullptr && field("discnumber")->values.front() == "1");
    CHECK(field("totaldiscs") != nullptr && field("totaldiscs")->values.front() == "2");
    CHECK(field("musicbrainztrackid") != nullptr &&
          field("musicbrainztrackid")->values.front() == "bbbb0000-0000-0000-0000-000000000001");
    CHECK(field("musicbrainzreleasetrackid") != nullptr);
    CHECK(field("musicbrainzalbumid") != nullptr &&
          field("musicbrainzalbumid")->values.front() == release.id);
    CHECK(field("musicbrainzreleasegroupid") != nullptr);
    CHECK(field("musicbrainzartistid") != nullptr &&
          field("musicbrainzartistid")->values ==
              (std::vector<std::string>{"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                                        "ffffffff-0000-1111-2222-333333333333"}));
    CHECK(field("title")->confidence > 0.9);
    CHECK(field("title")->rationale.find("Alpha") != std::string::npos);

    // Picard parity: sort names beside credited names, release-group
    // origin dates, lowercased type/status, and the release description
    // fields, plus per-track ISRCs and work ids.
    CHECK(field("artistsort") != nullptr &&
          field("artistsort")->values.front() == "Band, The feat. Guest");
    CHECK(field("albumartistsort") != nullptr &&
          field("albumartistsort")->values.front() == "Band, The feat. Guest");
    CHECK(field("originaldate") != nullptr &&
          field("originaldate")->values.front() == "1990-01-01");
    CHECK(field("originalyear") != nullptr && field("originalyear")->values.front() == "1990");
    CHECK(field("releasetype") != nullptr &&
          field("releasetype")->values == (std::vector<std::string>{"album", "live"}));
    CHECK(field("releasestatus") != nullptr &&
          field("releasestatus")->values.front() == "official");
    CHECK(field("releasecountry") != nullptr && field("releasecountry")->values.front() == "DE");
    CHECK(field("script") != nullptr && field("script")->values.front() == "Latn");
    CHECK(field("language") != nullptr && field("language")->values.front() == "eng");
    CHECK(field("label") != nullptr && field("label")->values.front() == "Label Records");
    CHECK(field("catalognumber") != nullptr && field("catalognumber")->values.front() == "CAT-123");
    // The empty barcode is never proposed.
    CHECK(field("barcode") == nullptr);
    CHECK(field("media") != nullptr && field("media")->values.front() == "CD");
    CHECK(field("discsubtitle") != nullptr &&
          field("discsubtitle")->values.front() == "First Night");
    CHECK(field("isrc") != nullptr &&
          field("isrc")->values == (std::vector<std::string>{"DEA119900001"}));
    CHECK(field("musicbrainzworkid") != nullptr &&
          field("musicbrainzworkid")->values ==
              (std::vector<std::string>{"cccc1111-0000-0000-0000-000000000001"}));

    // The disc-2 file gets its own numbering, no disc subtitle, and no
    // borrowed ISRC or work.
    const auto* third = &proposals->items.back();
    CHECK(third->item_index == 9U);
    for (const auto& value : third->fields) {
        if (value.canonical_field == "totaltracks") {
            CHECK(value.values.front() == "1");
        }
        if (value.canonical_field == "discnumber") {
            CHECK(value.values.front() == "2");
        }
        CHECK(value.canonical_field != "discsubtitle");
        CHECK(value.canonical_field != "isrc");
        CHECK(value.canonical_field != "musicbrainzworkid");
    }
}

void lowConfidenceTracksReceiveNothing() {
    const auto release = two_disc_release();
    const std::vector<LocalTrackDescriptor> locals{
        LocalTrackDescriptor{.title = "Entirely Unrelated",
                             .artist = {},
                             .album = {},
                             .track_number = {},
                             .disc_number = {},
                             .duration_ms = 500'000},
        LocalTrackDescriptor{.title = "One",
                             .artist = "Band",
                             .album = "Alpha",
                             .track_number = 1U,
                             .disc_number = 1U,
                             .duration_ms = 61'000},
    };
    const auto alignment = align_release_tracks(locals, release);
    const std::vector<std::size_t> items{0U, 1U};
    const auto proposals = release_metadata_proposals(release, alignment, items);
    CHECK(proposals.has_value());
    if (!proposals) {
        return;
    }
    for (const auto& item : proposals->items) {
        CHECK(item.item_index != 0U);
    }

    const std::vector<std::size_t> mismatched{1U};
    CHECK(!release_metadata_proposals(release, alignment, mismatched).has_value());
}

void manualMappingValidatesIdentityAndAllowsUntaggedFiles() {
    const auto release = two_disc_release();
    const std::vector<LocalTrackDescriptor> local(3);
    const auto initial = align_release_tracks(local, release);
    const std::vector<std::optional<std::size_t>> mapping{2U, std::nullopt, 0U};
    const auto confirmed = confirm_release_mapping(initial, mapping);
    CHECK(confirmed.has_value());
    if (!confirmed) {
        return;
    }
    CHECK(confirmed->matched_count == 2U);
    const std::vector<std::size_t> items{9U, 4U, 17U};
    const auto proposals = release_metadata_proposals(release, *confirmed, items);
    CHECK(proposals.has_value());
    if (!proposals) {
        return;
    }
    CHECK(proposals->items.size() == 2U);
    CHECK(proposals->items[0].item_index == 9U);
    CHECK(proposals->items[1].item_index == 17U);
    CHECK(proposals->items[0].fields.front().values == std::vector<std::string>{"Three"});
    CHECK(proposals->items[1].fields.front().values == std::vector<std::string>{"One"});
    CHECK(proposals->items[0].fields.front().rationale.starts_with("User-confirmed"));
    CHECK(!confirm_release_mapping(initial, std::vector<std::optional<std::size_t>>{0U, 0U, 1U}));
    CHECK(!confirm_release_mapping(initial, std::vector<std::optional<std::size_t>>{0U, 3U, 1U}));
    CHECK(!confirm_release_mapping(initial, std::vector<std::optional<std::size_t>>{0U}));
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    CHECK(align_release_tracks(local, release, cancellation.token()).tracks.empty());
}

// ADR-0261: an alignment says how it paired and how well, and only a
// numbered or ordered pairing with every length and title close is clear.
void clearMatchesNeedNoOne() {
    auto release = two_disc_release();
    release.media.pop_back();
    release.track_count = 2U;
    const auto file = [](std::string title, std::optional<std::size_t> number,
                         std::optional<std::int64_t> length) {
        return LocalTrackDescriptor{.title = std::move(title),
                                    .artist = {},
                                    .album = {},
                                    .track_number = number,
                                    .disc_number = {},
                                    .duration_ms = length};
    };
    const std::vector exact{file("One", 1U, 61'500), file("Two", 2U, 58'000)};
    const auto numbered = align_release_tracks(exact, release);
    CHECK(numbered.method == AlignmentMethod::numbers);
    CHECK(numbered.durations_compared == 2U);
    CHECK(numbered.worst_duration_delta_ms == 1'000);
    CHECK(numbered.weakest_title == 1.0);
    CHECK(is_clear_match(numbered, exact.size()));

    const std::vector unnumbered{file("One", {}, 61'000), file("Two", {}, 59'000)};
    const auto ordered = align_release_tracks(unnumbered, release);
    CHECK(ordered.method == AlignmentMethod::order);
    CHECK(is_clear_match(ordered, unnumbered.size()));

    const std::vector far{file("One", 1U, 61'000), file("Two", 2U, 70'000)};
    const auto too_long = align_release_tracks(far, release);
    CHECK(too_long.worst_duration_delta_ms == 11'000);
    CHECK(!is_clear_match(too_long, far.size()));
    CHECK(is_clear_match(too_long, far.size(),
                         ClearMatchRule{.maximum_duration_delta_ms = 12'000,
                                        .minimum_title_similarity = 0.8}));

    const std::vector unknown{file("One", 1U, 61'000), file("Two", 2U, {})};
    const auto no_length = align_release_tracks(unknown, release);
    CHECK(no_length.durations_compared == 1U);
    CHECK(!is_clear_match(no_length, unknown.size()));

    const std::vector renamed{file("One", 1U, 61'000), file("Something else", 2U, 59'000)};
    const auto unlike = align_release_tracks(renamed, release);
    CHECK(unlike.weakest_title < 0.8);
    CHECK(!is_clear_match(unlike, renamed.size()));

    const std::vector extra{file("Two", {}, 59'000), file("Stranger", {}, 200'000),
                            file("One", {}, 61'000)};
    const auto by_titles = align_release_tracks(extra, release);
    CHECK(by_titles.method == AlignmentMethod::titles);
    CHECK(!is_clear_match(by_titles, extra.size()));

    const std::vector one_short{file("One", 1U, 61'000)};
    CHECK(!is_clear_match(align_release_tracks(one_short, release), one_short.size()));
}

// ADR-0261: a selection's albums -- by release id, else tags across disc
// folders, else folder.
void selectionsGroupIntoAlbums() {
    const auto in = [](std::string path, std::string id, std::string album_artist,
                       std::string artist, std::string album, std::string date) {
        return AlbumGroupInput{.raw_path = std::move(path),
                               .release_id = std::move(id),
                               .album_artist = std::move(album_artist),
                               .artist = std::move(artist),
                               .album = std::move(album),
                               .date = std::move(date)};
    };
    const std::vector<AlbumGroupInput> files{
        in("/m/Wilson/Cover/CD 1/01.flac", "AAAA-1", "Steven Wilson", "", "Cover Version", "2014"),
        in("/m/Wilson/Cover/CD 2/01.flac", "aaaa-1", "Steven Wilson", "", "Cover Version", "2014"),
        in("/m/Schandmaul/Stern/01.flac", "", "Schandmaul", "", "Sternensegler", "2026-03-06"),
        in("/m/Schandmaul/Stern/CD 2/01.flac", "", "", "Schandmaul", "Sternensegler (CD 2)",
           "2026"),
        in("/m/Schandmaul/Old/01.flac", "", "Schandmaul", "", "Sternensegler", "2001"),
        in("/m/incoming/rip-0412/CD 1/track01.flac", "", "", "", "", ""),
        in("/m/incoming/rip-0412/CD 2/track01.flac", "", "", "", "", ""),
        in("/m/incoming/other/track01.flac", "", "", "", "  ", ""),
    };
    const auto groups = group_albums(files);
    CHECK(groups.size() == 5U);
    CHECK(groups[0].basis == AlbumGroupBasis::release_id);
    CHECK(groups[0].items == (std::vector<std::size_t>{0U, 1U}));
    CHECK(groups[0].folders ==
          (std::vector<std::string>{"/m/Wilson/Cover/CD 1", "/m/Wilson/Cover/CD 2"}));
    CHECK(groups[1].basis == AlbumGroupBasis::tags);
    CHECK(groups[1].items == (std::vector<std::size_t>{2U, 3U}));
    CHECK(groups[2].items == (std::vector<std::size_t>{4U}));
    CHECK(groups[3].basis == AlbumGroupBasis::folder);
    CHECK(groups[3].key == "/m/incoming/rip-0412");
    CHECK(groups[3].items == (std::vector<std::size_t>{5U, 6U}));
    CHECK(groups[4].key == "/m/incoming/other");

    CHECK(is_disc_folder("CD 2") && is_disc_folder("disc1") && is_disc_folder("Disk_03") &&
          is_disc_folder("cd-12"));
    CHECK(!is_disc_folder("CD") && !is_disc_folder("Discography") && !is_disc_folder("CD 2 bonus") &&
          !is_disc_folder("2"));
}

} // namespace

int main() {
    manualMappingValidatesIdentityAndAllowsUntaggedFiles();
    rankingCorroboratesTrackCount();
    alignsByDiscAndTrackNumberPermutation();
    alignsByOrderAndThenGreedyTitles();
    proposalsCarryTagsIdentifiersAndVersion();
    lowConfidenceTracksReceiveNothing();
    clearMatchesNeedNoOne();
    selectionsGroupIntoAlbums();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
