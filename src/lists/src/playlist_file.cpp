// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/lists/playlist_file.hpp"
#include "trackknife/core/unicode.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>

namespace trackknife::lists {
namespace {

constexpr std::size_t max_bytes = 32U * 1024U * 1024U;
constexpr std::size_t max_entries = 100'000U;
constexpr std::string_view melody_entry = "#MELODY:";
constexpr std::string_view melody_list = "#MELODY-LIST:";

[[nodiscard]] std::string folderOf(const std::string& path) {
    return path.substr(0, path.find_last_of('/') + 1);
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string result{text};
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return result;
}

// What is not UTF-8 is taken as Latin-1, as old .m3u files are.
[[nodiscard]] std::string readable(std::string_view text) {
    if (core::unicodeCodePointCount(text)) {
        return std::string{text};
    }
    std::string result;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x80U) {
            result += static_cast<char>(c);
        } else {
            result += static_cast<char>(0xC0U | (c >> 6U));
            result += static_cast<char>(0x80U | (c & 0x3FU));
        }
    }
    return result;
}

[[nodiscard]] int hexDigit(const char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A local file URI's path; none for any other.
[[nodiscard]] std::optional<std::string> fileUriPath(std::string_view value) {
    value.remove_prefix(5);
    if (value.starts_with("//localhost/")) {
        value.remove_prefix(11);
    } else if (value.starts_with("///")) {
        value.remove_prefix(2);
    } else if (value.starts_with("//")) {
        return std::nullopt;
    }
    std::string path;
    for (std::size_t at = 0; at < value.size(); ++at) {
        if (value[at] != '%') {
            path += value[at];
            continue;
        }
        if (at + 2 >= value.size() || hexDigit(value[at + 1]) < 0 || hexDigit(value[at + 2]) < 0) {
            return std::nullopt;
        }
        path += static_cast<char>(hexDigit(value[at + 1]) * 16 + hexDigit(value[at + 2]));
        at += 2;
    }
    return path.starts_with('/') ? std::optional{path} : std::nullopt;
}

[[nodiscard]] std::string fileUri(std::string_view path) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string result{"file://"};
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += digits[c >> 4U];
            result += digits[c & 15U];
        }
    }
    return result;
}

[[nodiscard]] std::string normal(const std::string& path) {
    return std::filesystem::path{path}.lexically_normal().string();
}

// `relative` under `root`.
[[nodiscard]] std::string under(std::string root, const std::string& relative) {
    root += '/';
    root += relative;
    return normal(root);
}

// Where a line came from, to tell the file's style.
enum class Anchor : std::uint8_t { absolute, playlist, root, unknown };

struct Reference {
    std::string path;
    Anchor anchor{Anchor::absolute};
    // An unknown one's line, placed once the file's style is known.
    std::string relative;
};

[[nodiscard]] std::optional<Reference>
reference(std::string_view line, const std::string& folder,
          std::span<const std::string> music_roots,
          const std::function<bool(const std::string&)>& exists) {
    if (lowered(line.substr(0, 5)) == "file:") {
        auto path = fileUriPath(line);
        if (!path) {
            return std::nullopt;
        }
        return Reference{.path = normal(*path), .anchor = Anchor::absolute, .relative = {}};
    }
    // A web address, a Windows drive: not a file here.
    const auto colon = line.find(':');
    const auto slash = line.find('/');
    if ((colon != std::string_view::npos && (slash == std::string_view::npos || colon < slash)) ||
        line.find('\\') != std::string_view::npos) {
        return std::nullopt;
    }
    if (line.starts_with('/')) {
        return Reference{
            .path = normal(std::string{line}), .anchor = Anchor::absolute, .relative = {}};
    }
    std::string relative{line};
    if (const auto beside = normal(folder + relative); exists(beside)) {
        return Reference{.path = beside, .anchor = Anchor::playlist, .relative = {}};
    }
    // Under a music folder, as MPD names them -- never climbing out of it.
    for (const auto& root :
         relative.starts_with("../") ? std::span<const std::string>{} : music_roots) {
        if (auto found = under(root, relative); exists(found)) {
            return Reference{.path = std::move(found), .anchor = Anchor::root, .relative = {}};
        }
    }
    return Reference{.path = {}, .anchor = Anchor::unknown, .relative = std::move(relative)};
}

[[nodiscard]] std::optional<std::int64_t> durationOf(std::string_view seconds_text) {
    double seconds = 0;
    const auto parsed =
        std::from_chars(seconds_text.data(), seconds_text.data() + seconds_text.size(), seconds);
    if (parsed.ec != std::errc{} || !std::isfinite(seconds) || seconds < 0 || seconds > 1.0e9) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1000));
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::optional<PlaylistFormat> playlist_format(std::string_view file_name) {
    const auto name = lowered(file_name);
    if (name.ends_with(".m3u") || name.ends_with(".m3u8")) {
        return PlaylistFormat::m3u;
    }
    if (name.ends_with(".pls")) {
        return PlaylistFormat::pls;
    }
    return std::nullopt;
}

core::Result<PlaylistFile>
parse_playlist_file(std::string_view bytes, const std::string& absolute_path,
                    std::span<const std::string> music_roots,
                    const std::function<bool(const std::string&)>& exists) {
    if (bytes.size() > max_bytes) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "the playlist is over 32 MiB",
                                           .context = {}});
    }
    if (bytes.starts_with("\xef\xbb\xbf")) {
        bytes.remove_prefix(3);
    }
    const auto folder = folderOf(absolute_path);
    PlaylistFile file;
    std::vector<Reference> references;
    const auto add = [&](std::string_view line, PlaylistFileEntry entry) {
        auto found = reference(line, folder, music_roots, exists);
        if (!found || references.size() == max_entries) {
            ++file.skipped;
            return;
        }
        references.push_back(std::move(*found));
        file.entries.push_back(std::move(entry));
    };
    const auto lines = [&bytes](const auto& each) {
        while (!bytes.empty()) {
            const auto end = bytes.find('\n');
            auto line = bytes.substr(0, end);
            bytes.remove_prefix(end == std::string_view::npos ? bytes.size() : end + 1);
            each(trimmed(line));
        }
    };

    if (playlist_format(absolute_path) == PlaylistFormat::pls) {
        // FileN, TitleN, LengthN -- in N's order, whatever the lines' are.
        struct Numbered {
            std::string file;
            std::string title;
            std::optional<std::int64_t> duration_ms;
        };
        std::map<long, Numbered> numbered;
        lines([&numbered](std::string_view line) {
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) {
                return;
            }
            const auto key = lowered(line.substr(0, equals));
            const auto value = line.substr(equals + 1);
            for (const std::string_view prefix : {"file", "title", "length"}) {
                if (!key.starts_with(prefix)) {
                    continue;
                }
                long number = 0;
                const auto digits = std::string_view{key}.substr(prefix.size());
                const auto parsed =
                    std::from_chars(digits.data(), digits.data() + digits.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
                    return;
                }
                auto& entry = numbered[number];
                if (prefix == "file") {
                    entry.file = value;
                } else if (prefix == "title") {
                    entry.title = readable(value);
                } else {
                    entry.duration_ms = durationOf(value);
                }
                return;
            }
        });
        for (auto& [number, entry] : numbered) {
            if (entry.file.empty()) {
                continue;
            }
            add(entry.file, PlaylistFileEntry{.raw_path = {},
                                              .title = std::move(entry.title),
                                              .duration_ms = entry.duration_ms,
                                              .melody = {}});
        }
    } else {
        PlaylistFileEntry pending;
        lines([&](std::string_view line) {
            if (line.empty()) {
                return;
            }
            if (line.starts_with(melody_list)) {
                file.list_id = trimmed(line.substr(melody_list.size()));
                return;
            }
            if (line.starts_with(melody_entry)) {
                pending.melody = trimmed(line.substr(melody_entry.size()));
                return;
            }
            if (line.starts_with("#EXTINF:")) {
                const auto comma = line.find(',');
                const auto seconds = line.substr(
                    8, comma == std::string_view::npos ? std::string_view::npos : comma - 8);
                pending.duration_ms = durationOf(trimmed(seconds));
                pending.title = comma == std::string_view::npos ? std::string{}
                                                                : readable(line.substr(comma + 1));
                return;
            }
            if (line.starts_with('#')) {
                return;
            }
            add(line, std::move(pending));
            pending = {};
        });
    }

    // The file's style: how most of its lines were found. A line found
    // nowhere -- its track gone -- is placed as the rest are.
    const auto count = [&references](const Anchor anchor) {
        return std::ranges::count(references, anchor, &Reference::anchor);
    };
    const auto beside = count(Anchor::playlist);
    const auto rooted = count(Anchor::root);
    const auto absolute = count(Anchor::absolute);
    file.style = rooted > beside && !music_roots.empty() ? PlaylistPathStyle::root_relative
                 : beside + rooted + count(Anchor::unknown) == 0 && absolute > 0
                     ? PlaylistPathStyle::absolute
                     : PlaylistPathStyle::playlist_relative;
    for (std::size_t at = 0; at < references.size(); ++at) {
        auto& found = references[at];
        if (found.anchor == Anchor::unknown) {
            found.path = file.style == PlaylistPathStyle::root_relative
                             ? under(music_roots.front(), found.relative)
                             : normal(folder + found.relative);
        }
        file.entries[at].raw_path = std::move(found.path);
    }
    return file;
}

std::string render_playlist_file(std::span<const PlaylistFileEntry> entries,
                                 const std::string& absolute_path, const PlaylistPathStyle style,
                                 std::span<const std::string> music_roots,
                                 const std::string_view list_id) {
    const auto folder = folderOf(absolute_path);
    const auto named = [&](const std::string& path) -> std::string {
        std::string line = path;
        if (style == PlaylistPathStyle::root_relative) {
            // Under the longest music folder holding it.
            std::string_view best;
            for (const auto& root : music_roots) {
                const auto prefix = root.ends_with('/') ? root : root + '/';
                if (path.starts_with(prefix) && prefix.size() > best.size()) {
                    best = root;
                    line = path.substr(prefix.size());
                }
            }
        } else if (style == PlaylistPathStyle::playlist_relative) {
            const auto relative = std::filesystem::path{path}
                                      .lexically_relative(folder.substr(0, folder.size() - 1))
                                      .string();
            if (!relative.empty()) {
                line = relative;
            }
        }
        // What a line cannot hold as it is -- a line break, a leading space
        // or '#' -- goes as a file URI of the whole path.
        if (line.find_first_of("\r\n") != std::string::npos || line.starts_with(' ') ||
            line.starts_with('\t') || line.starts_with('#') ||
            lowered(std::string_view{line}.substr(0, 5)) == "file:") {
            return fileUri(path);
        }
        return line;
    };
    const auto oneLine = [](std::string text) {
        std::ranges::replace(text, '\n', ' ');
        std::ranges::replace(text, '\r', ' ');
        return text;
    };
    const auto seconds = [](const std::optional<std::int64_t>& duration_ms) {
        return duration_ms ? std::to_string((*duration_ms + 500) / 1000) : std::string{"-1"};
    };
    std::string out;
    if (playlist_format(absolute_path) == PlaylistFormat::pls) {
        out += "[playlist]\n";
        std::size_t number = 0;
        for (const auto& entry : entries) {
            const auto n = std::to_string(++number);
            out += "File" + n + "=" + oneLine(named(entry.raw_path)) + "\n";
            if (!entry.title.empty()) {
                out += "Title" + n + "=" + oneLine(entry.title) + "\n";
            }
            out += "Length" + n + "=" + seconds(entry.duration_ms) + "\n";
        }
        out += "NumberOfEntries=" + std::to_string(number) + "\nVersion=2\n";
        return out;
    }
    out += "#EXTM3U\n";
    if (!list_id.empty()) {
        out += std::string{melody_list} + std::string{list_id} + "\n";
    }
    for (const auto& entry : entries) {
        if (!entry.melody.empty()) {
            out += std::string{melody_entry} + oneLine(entry.melody) + "\n";
        }
        if (!entry.title.empty() || entry.duration_ms) {
            out += "#EXTINF:" + seconds(entry.duration_ms) + "," + oneLine(entry.title) + "\n";
        }
        out += named(entry.raw_path) + "\n";
    }
    return out;
}

} // namespace trackknife::lists
