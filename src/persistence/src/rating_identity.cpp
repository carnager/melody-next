// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/rating_identity.hpp"

#include "trackknife/core/sha256.hpp"

#include <charconv>
#include <filesystem>
#include <string>

namespace trackknife::persistence {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto begin = text.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(whitespace);
    return std::string{text.substr(begin, end - begin + 1U)};
}

[[nodiscard]] std::string joined(std::initializer_list<std::string_view> parts) {
    std::string bytes;
    bool first = true;
    for (const auto part : parts) {
        if (!first) {
            bytes.push_back('\0');
        }
        first = false;
        bytes.append(part);
    }
    return bytes;
}

} // namespace

std::string track_rating_hash(const std::string_view album_artist, const std::string_view album,
                              const std::string_view title, const int track_number) {
    return core::sha256_hex(
        joined({album_artist, album, title, std::to_string(track_number)}));
}

std::string album_rating_hash(const std::string_view album_artist, const std::string_view album,
                              const std::string_view date) {
    return core::sha256_hex(joined({album_artist, album, date}));
}

RatingIdentity rating_identity(const metadata::MetadataDocument& document,
                               const std::string& raw_path) {
    const auto value = [&](std::string_view name) {
        return trimmed(document.first_effective_value(name).value_or(""));
    };
    const std::filesystem::path path{raw_path};

    auto album_artist = value("albumartist");
    if (album_artist.empty()) {
        album_artist = value("artist");
    }
    if (album_artist.empty()) {
        album_artist = "Unknown Artist";
    }
    auto album = value("album");
    if (album.empty()) {
        album = path.parent_path().filename().native();
    }
    auto title = value("title");
    if (title.empty()) {
        title = path.stem().native();
    }
    const auto track_text = value("tracknumber");
    int track_number = 0;
    std::from_chars(track_text.data(), track_text.data() + track_text.size(), track_number);

    const auto date_text = value("date");
    int year = 0;
    std::from_chars(date_text.data(), date_text.data() + date_text.size(), year);
    const auto date = year > 0 ? std::to_string(year) : std::string{"0000"};

    return {
        .track_hash = track_rating_hash(album_artist, album, title, track_number),
        .album_hash = album_rating_hash(album_artist, album, date),
    };
}

} // namespace trackknife::persistence
