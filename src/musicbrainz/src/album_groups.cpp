// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/album_groups.hpp"

#include "trackknife/core/unicode.hpp"
#include "trackknife/loudness/grouping.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string{text};
}

[[nodiscard]] std::string folded(std::string_view text) {
    auto lowered = core::unicodeSimpleLower(trimmed(text));
    return lowered ? std::move(*lowered) : trimmed(text);
}

[[nodiscard]] std::string parent_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
}

[[nodiscard]] std::string_view name_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1U);
}

// The year a date begins with, else the date as it is.
[[nodiscard]] std::string year_of(std::string_view date) {
    const auto text = trimmed(date);
    if (text.size() >= 4U && std::all_of(text.begin(), text.begin() + 4,
                                         [](const char c) { return c >= '0' && c <= '9'; })) {
        return text.substr(0, 4);
    }
    return text;
}

} // namespace

bool is_disc_folder(const std::string_view name) {
    auto lowered = folded(name);
    std::string_view rest{lowered};
    bool named = false;
    for (const std::string_view word : {"disc", "disk", "cd"}) {
        if (rest.starts_with(word)) {
            rest.remove_prefix(word.size());
            named = true;
            break;
        }
    }
    if (!named) {
        return false;
    }
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '_' || rest.front() == '-')) {
        rest.remove_prefix(1);
    }
    return !rest.empty() && rest.size() <= 3U &&
           std::ranges::all_of(rest, [](const char c) { return c >= '0' && c <= '9'; });
}

std::vector<AlbumGroup> group_albums(const std::span<const AlbumGroupInput> files) {
    std::vector<AlbumGroup> groups;
    std::map<std::string, std::size_t> found;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        const auto folder = parent_of(file.raw_path);
        AlbumGroupBasis basis{};
        std::string key;
        if (!trimmed(file.release_id).empty()) {
            basis = AlbumGroupBasis::release_id;
            key = "mbid:" + folded(file.release_id);
        } else if (!trimmed(file.album).empty()) {
            basis = AlbumGroupBasis::tags;
            const auto& artist = trimmed(file.album_artist).empty() ? file.artist : file.album_artist;
            key = "tag:" + folded(artist) + '\x1F' +
                  folded(loudness::strip_disc_designator(trimmed(file.album))) + '\x1F' +
                  year_of(file.date);
        } else {
            basis = AlbumGroupBasis::folder;
            key = is_disc_folder(name_of(folder)) ? parent_of(folder) : folder;
        }
        const auto [at, added] = found.emplace(key, groups.size());
        if (added) {
            groups.push_back(AlbumGroup{.basis = basis, .key = key, .items = {}, .folders = {}});
        }
        auto& group = groups[at->second];
        group.items.push_back(index);
        if (std::ranges::find(group.folders, folder) == group.folders.end()) {
            group.folders.push_back(folder);
        }
    }
    return groups;
}

} // namespace trackknife::musicbrainz
