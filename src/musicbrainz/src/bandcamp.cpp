// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/bandcamp.hpp"

#include <algorithm>
#include <cctype>

namespace trackknife::musicbrainz {
namespace {

// The host of an https URL, lower-cased; empty for anything else.
[[nodiscard]] std::string https_host(const std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return {};
    }
    const auto rest = url.substr(scheme.size());
    auto host = std::string{rest.substr(0, rest.find_first_of("/?#"))};
    std::ranges::transform(host, host.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // No user, no port: a name and nothing else.
    if (host.empty() || host.find_first_of("@:") != std::string::npos) {
        return {};
    }
    return host;
}

[[nodiscard]] std::string_view path_of(const std::string_view url) {
    const auto rest = url.substr(std::string_view{"https://"}.size());
    const auto slash = rest.find('/');
    return slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
}

[[nodiscard]] bool plain_label(const std::string_view label) {
    return !label.empty() && std::ranges::all_of(label, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

} // namespace

bool is_bandcamp_album_page(const std::string_view url) {
    const auto host = https_host(url);
    constexpr std::string_view domain = ".bandcamp.com";
    if (!host.ends_with(domain) ||
        !plain_label(std::string_view{host}.substr(0, host.size() - domain.size()))) {
        return false;
    }
    const auto path = path_of(url);
    return path.starts_with("/album/") && path.size() > 7U;
}

bool is_bandcamp_image(const std::string_view url) {
    const auto host = https_host(url);
    constexpr std::string_view domain = ".bcbits.com";
    return host.ends_with(domain) &&
           plain_label(std::string_view{host}.substr(0, host.size() - domain.size())) &&
           path_of(url).starts_with("/img/");
}

std::optional<std::string> bandcamp_album_page(const std::vector<std::string>& urls) {
    for (auto url : urls) {
        // MusicBrainz keeps some as http://.
        if (url.starts_with("http://")) {
            url = "https://" + url.substr(7);
        }
        if (is_bandcamp_album_page(url)) {
            return url;
        }
    }
    return std::nullopt;
}

std::optional<BandcampCoverUrls> bandcamp_cover_urls(const std::string_view page) {
    // <meta property="og:image" content="https://f4.bcbits.com/img/a123_5.jpg">
    constexpr std::string_view property = "property=\"og:image\"";
    const auto at = page.find(property);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto tag_start = page.rfind('<', at);
    const auto tag_end = page.find('>', at);
    if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto tag = page.substr(tag_start, tag_end - tag_start);
    constexpr std::string_view content = "content=\"";
    const auto value = tag.find(content);
    if (value == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = value + content.size();
    const auto end = tag.find('"', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string url{tag.substr(start, end - start)};
    if (!is_bandcamp_image(url)) {
        return std::nullopt;
    }
    // Its size is the number after the last underscore: 10 is 1200 pixels,
    // 2 is 350.
    const auto underscore = url.rfind('_');
    const auto dot = url.rfind('.');
    if (underscore == std::string::npos || dot == std::string::npos || dot < underscore) {
        return std::nullopt;
    }
    const auto sized = [&](const std::string_view size) {
        return url.substr(0, underscore + 1U) + std::string{size} + url.substr(dot);
    };
    return BandcampCoverUrls{.image = sized("10"), .thumbnail = sized("2")};
}

} // namespace trackknife::musicbrainz
