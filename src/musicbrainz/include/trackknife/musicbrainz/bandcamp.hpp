// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::musicbrainz {

// ADR-0326: Bandcamp as a cover source -- found through a release's
// MusicBrainz links, never by searching. Qt-free: an engine fetches only
// what these allow.

// A Bandcamp album page: https://NAME.bandcamp.com/album/SLUG.
[[nodiscard]] bool is_bandcamp_album_page(std::string_view url);
// A Bandcamp image: https://NAME.bcbits.com/img/....
[[nodiscard]] bool is_bandcamp_image(std::string_view url);
// The first Bandcamp album page among links, as asked for over HTTPS.
[[nodiscard]] std::optional<std::string> bandcamp_album_page(const std::vector<std::string>& urls);

struct BandcampCoverUrls {
    // 1200 pixels, and a 350-pixel preview.
    std::string image;
    std::string thumbnail;
};
// The cover a Bandcamp album page names: its og:image, at those sizes.
[[nodiscard]] std::optional<BandcampCoverUrls> bandcamp_cover_urls(std::string_view page);

} // namespace trackknife::musicbrainz
