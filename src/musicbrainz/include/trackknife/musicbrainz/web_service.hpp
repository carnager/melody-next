// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::musicbrainz {

// MusicBrainz asks for at most one request per second per client plus an
// identifying User-Agent; the client paces slightly slower than required.
inline constexpr int minimum_request_interval_ms = 1'100;
// Release data changes slowly; cached web-service responses stay valid for
// two weeks and the cache stays bounded.
inline constexpr std::int64_t response_cache_ttl_seconds = 14LL * 24 * 60 * 60;
inline constexpr std::size_t response_cache_maximum_entries = 10'000U;

struct WebServiceLimits {
    std::size_t releases{25U};
    std::size_t media{64U};
    std::size_t tracks_per_medium{256U};
    std::size_t artist_credits{32U};
    std::size_t identifiers{16U};
    std::size_t cover_art_images{100U};
    std::size_t text_bytes{2'048U};
    std::size_t body_bytes{8U * 1024U * 1024U};
};

struct ReleaseSearchQuery {
    std::string artist;
    std::string release;
    std::optional<std::size_t> track_count;
    std::size_t limit{8U};
};

// Builds the ws/2 release-search URL with Lucene phrase escaping and URL
// encoding. At least one of artist/release must be non-empty.
[[nodiscard]] core::Result<std::string> build_release_search_url(const ReleaseSearchQuery& query);

// Builds the ws/2 release lookup URL including artist credits, recordings,
// release groups, labels, ISRCs, and recording-level work relations. The id
// must be a MusicBrainz UUID.
[[nodiscard]] core::Result<std::string> build_release_lookup_url(std::string_view release_id);

// The release a person names: a bare release id, or a MusicBrainz link to a
// release (musicbrainz.org/release/<id>, any page of it) -- lowercased, as
// MusicBrainz writes ids. None for anything else, a release group's link
// included: it names an album, not one of its versions.
[[nodiscard]] std::optional<std::string> release_id_in(std::string_view text);
// The release group a MusicBrainz link names (musicbrainz.org/release-group/
// <id>); none for a bare id, which release_id_in takes as a release's.
[[nodiscard]] std::optional<std::string> release_group_id_in(std::string_view text);

// How many of a release group's releases are listed: one page.
inline constexpr std::size_t release_group_releases_limit = 100U;
// Builds the ws/2 URL listing a release group's releases -- every version
// of an album -- with their media, labels and artist credits, the first
// release_group_releases_limit of them. Parsed by parse_release_search,
// with limits allowing that many.
[[nodiscard]] core::Result<std::string> build_release_group_releases_url(std::string_view group_id);

struct ArtistCredit {
    std::string name;
    std::string join_phrase;
    std::string artist_id;
    std::string sort_name;

    friend bool operator==(const ArtistCredit&, const ArtistCredit&) = default;
};

struct ReleaseTrack {
    std::string track_id;
    std::string recording_id;
    std::size_t position{0U};
    std::string number;
    std::string title;
    std::optional<std::int64_t> length_ms;
    std::vector<ArtistCredit> artist_credits;
    std::vector<std::string> isrcs;
    std::vector<std::string> work_ids;

    friend bool operator==(const ReleaseTrack&, const ReleaseTrack&) = default;
};

struct ReleaseMedium {
    std::size_t position{0U};
    std::string format;
    std::string title;
    std::size_t track_count{0U};
    std::vector<ReleaseTrack> tracks;

    friend bool operator==(const ReleaseMedium&, const ReleaseMedium&) = default;
};

struct Release {
    std::string id;
    std::string title;
    std::string status;
    std::string disambiguation;
    std::string date;
    std::string country;
    std::string barcode;
    std::string release_group_id;
    std::string release_group_primary_type;
    std::vector<std::string> release_group_secondary_types;
    std::string release_group_first_release_date;
    std::string script;
    std::string language;
    std::string label;
    std::string catalog_number;
    int search_score{0};
    std::size_t track_count{0U};
    std::vector<ArtistCredit> artist_credits;
    std::vector<ReleaseMedium> media;

    friend bool operator==(const Release&, const Release&) = default;
};

struct ReleaseSearchResult {
    std::size_t total_count{0U};
    std::vector<Release> releases;

    friend bool operator==(const ReleaseSearchResult&, const ReleaseSearchResult&) = default;
};

// Parses a ws/2 fmt=json release-search payload, or a release group's
// listing of its releases. Malformed JSON, missing
// release identities, and exceeded limits fail typed; optional detail that
// is absent or mistyped is dropped, never invented.
[[nodiscard]] core::Result<ReleaseSearchResult>
parse_release_search(std::string_view body, const WebServiceLimits& limits = {});

// Parses a ws/2 fmt=json release lookup including per-medium track listings.
[[nodiscard]] core::Result<Release> parse_release_lookup(std::string_view body,
                                                         const WebServiceLimits& limits = {});

struct CoverArtImage {
    std::string id;
    bool front{false};
    bool back{false};
    bool approved{false};
    std::string comment;
    std::vector<std::string> types;
    std::string image_url;
    // Smallest useful preview the archive offers (250px, else "small").
    std::string thumbnail_url;
    // ADR-0326: where it is from when not the Cover Art Archive
    // ("Bandcamp"); empty for the archive's own.
    std::string source;

    friend bool operator==(const CoverArtImage&, const CoverArtImage&) = default;
};

struct CoverArtListing {
    std::vector<CoverArtImage> images;

    friend bool operator==(const CoverArtListing&, const CoverArtListing&) = default;
};

// Builds the Cover Art Archive listing URL for one release. The id must be
// a MusicBrainz UUID.
[[nodiscard]] core::Result<std::string> build_cover_art_listing_url(std::string_view release_id);

// ADR-0332: the Cover Art Archive's listing for a release group -- the front
// cover of the release MusicBrainz shows for the album, for a release of it
// that has none of its own. Parsed by parse_cover_art_listing.
[[nodiscard]] core::Result<std::string>
build_release_group_cover_art_listing_url(std::string_view group_id);

// Parses a Cover Art Archive release listing. Image URLs are upgraded to
// https so the transport never follows a less-safe redirect; entries with
// no usable image URL are dropped, never invented.
[[nodiscard]] core::Result<CoverArtListing>
parse_cover_art_listing(std::string_view body, const WebServiceLimits& limits = {});

// Deterministic front-cover choice: the first image flagged front, else the
// first typed "Front", else the first approved image, else the first image.
[[nodiscard]] std::optional<std::size_t> select_front_cover(const CoverArtListing& listing);

// ADR-0326: a release's links (its URL relationships) and its album's id,
// for the album's links too.
struct ReleaseLinks {
    std::vector<std::string> urls;
    std::string release_group_id;

    friend bool operator==(const ReleaseLinks&, const ReleaseLinks&) = default;
};
[[nodiscard]] core::Result<std::string> build_release_links_url(std::string_view release_id);
[[nodiscard]] core::Result<std::string> build_release_group_links_url(std::string_view group_id);
// A release's or a release group's lookup with its links.
[[nodiscard]] core::Result<ReleaseLinks> parse_release_links(std::string_view body,
                                                             const WebServiceLimits& limits = {});
// The cover a Bandcamp album page names (its og:image), as an image of the
// listing: front, 1200 pixels, a 350-pixel preview.
[[nodiscard]] std::optional<CoverArtImage> bandcamp_cover(std::string_view page);

} // namespace trackknife::musicbrainz
