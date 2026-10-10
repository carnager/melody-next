// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/lyrics.hpp"

#include "trackknife/core/local_sources.hpp"

#include <fileref.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <synchronizedlyricsframe.h>
#include <tfile.h>
#include <tfilestream.h>
#include <tpropertymap.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace trackknife::metadata {
namespace {

constexpr std::uintmax_t sidecar_limit = 1024U * 1024U;
constexpr std::size_t directory_limit = 10'000U;

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    constexpr std::string_view space = " \t\r\n";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1U);
}

[[nodiscard]] bool digits(const std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](const char c) {
        return c >= '0' && c <= '9';
    });
}

[[nodiscard]] std::int64_t number(const std::string_view text) {
    std::int64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// mm:ss, mm:ss.f, mm:ss.ff, mm:ss.fff, or mm:ss:ff; nullopt when it is none.
[[nodiscard]] std::optional<std::int64_t> timing(const std::string_view tag) {
    const auto colon = tag.find(':');
    if (colon == std::string_view::npos || !digits(tag.substr(0, colon))) {
        return std::nullopt;
    }
    auto rest = tag.substr(colon + 1U);
    const auto separator = rest.find_first_of(".:");
    const auto seconds = rest.substr(0, separator);
    if (!digits(seconds) || seconds.size() > 2U || number(seconds) >= 60) {
        return std::nullopt;
    }
    std::int64_t fraction_ms = 0;
    if (separator != std::string_view::npos) {
        const auto fraction = rest.substr(separator + 1U);
        if (!digits(fraction) || fraction.size() > 3U) {
            return std::nullopt;
        }
        fraction_ms = number(fraction);
        for (auto size = fraction.size(); size < 3U; ++size) {
            fraction_ms *= 10;
        }
    }
    return number(tag.substr(0, colon)) * 60'000 + number(seconds) * 1'000 + fraction_ms;
}

// The text without its word timings: <mm:ss.xx>.
[[nodiscard]] std::string without_word_timings(std::string_view text) {
    std::string result;
    while (!text.empty()) {
        const auto open = text.find('<');
        if (open == std::string_view::npos) {
            result.append(text);
            break;
        }
        const auto close = text.find('>', open);
        if (close == std::string_view::npos || !timing(text.substr(open + 1U, close - open - 1U))) {
            result.append(text.substr(0, open + 1U));
            text.remove_prefix(open + 1U);
            continue;
        }
        result.append(text.substr(0, open));
        text.remove_prefix(close + 1U);
    }
    return std::string{trimmed(result)};
}

[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3U);
    }
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (line.ends_with('\r')) {
            line.remove_suffix(1U);
        }
        lines.push_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1U);
    }
    return lines;
}

[[nodiscard]] std::string lower(std::string text) {
    for (auto& byte : text) {
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return text;
}

[[nodiscard]] std::optional<std::string> read_small_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > sidecar_limit) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input{path, std::ios::binary};
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) && !bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] bool valid_utf8(const std::string_view text) {
    std::size_t i = 0U;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0U;
        if (byte < 0x80U) {
            length = 1U;
        } else if ((byte & 0xE0U) == 0xC0U && byte >= 0xC2U) {
            length = 2U;
        } else if ((byte & 0xF0U) == 0xE0U) {
            length = 3U;
        } else if ((byte & 0xF8U) == 0xF0U && byte <= 0xF4U) {
            length = 4U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1U; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

// An .lrc that is not UTF-8 is, almost always, Latin-1 from an older tool.
[[nodiscard]] std::string as_utf8(std::string text) {
    if (valid_utf8(text)) {
        return text;
    }
    std::string converted;
    converted.reserve(text.size() * 2U);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80U) {
            converted.push_back(c);
        } else {
            converted.push_back(static_cast<char>(0xC0U | (byte >> 6U)));
            converted.push_back(static_cast<char>(0x80U | (byte & 0x3FU)));
        }
    }
    return converted;
}

[[nodiscard]] bool has_lines(const Lyrics& lyrics) {
    return std::ranges::any_of(lyrics.lines,
                               [](const LyricsLine& line) { return !line.text.empty(); });
}

} // namespace

Lyrics parse_lyrics(const std::string_view text) {
    Lyrics synced{.source = {}, .synced = true, .lines = {}};
    Lyrics plain;
    std::int64_t offset_ms = 0;
    for (const auto raw : lines_of(text)) {
        auto line = trimmed(raw);
        std::vector<std::int64_t> times;
        bool tagged = false;
        while (line.starts_with('[')) {
            const auto close = line.find(']');
            if (close == std::string_view::npos) {
                break;
            }
            const auto tag = line.substr(1U, close - 1U);
            if (const auto time = timing(tag)) {
                times.push_back(*time);
            } else if (const auto colon = tag.find(':');
                       colon != std::string_view::npos && colon > 0U &&
                       std::ranges::all_of(tag.substr(0, colon), [](const char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                       })) {
                if (lower(std::string{tag.substr(0, colon)}) == "offset") {
                    auto value = trimmed(tag.substr(colon + 1U));
                    const bool negative = value.starts_with('-');
                    if (negative || value.starts_with('+')) {
                        value.remove_prefix(1U);
                    }
                    if (digits(value)) {
                        offset_ms = negative ? -number(value) : number(value);
                    }
                }
            } else {
                break;
            }
            tagged = true;
            line = trimmed(line.substr(close + 1U));
        }
        if (!times.empty()) {
            auto words = without_word_timings(line);
            for (const auto time : times) {
                synced.lines.push_back({.time_ms = time, .text = words});
            }
        } else if (!tagged) {
            plain.lines.push_back({.time_ms = std::nullopt, .text = std::string{line}});
        }
    }
    if (!synced.lines.empty()) {
        // A positive offset shows the lines sooner.
        for (auto& line : synced.lines) {
            line.time_ms = std::max<std::int64_t>(0, *line.time_ms - offset_ms);
        }
        std::ranges::stable_sort(synced.lines, {}, [](const LyricsLine& line) {
            return *line.time_ms;
        });
        return synced;
    }
    while (!plain.lines.empty() && plain.lines.back().text.empty()) {
        plain.lines.pop_back();
    }
    while (!plain.lines.empty() && plain.lines.front().text.empty()) {
        plain.lines.erase(plain.lines.begin());
    }
    return plain;
}

std::optional<std::string> lyrics_sidecar_path(const std::string& raw_audio_path) {
    const std::filesystem::path audio{raw_audio_path};
    auto exact = audio;
    exact.replace_extension(".lrc");
    std::error_code error;
    if (std::filesystem::is_regular_file(exact, error)) {
        return exact.native();
    }
    const auto wanted = lower(exact.filename().native());
    std::filesystem::directory_iterator iterator{
        audio.parent_path(), std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        return std::nullopt;
    }
    std::size_t visited = 0U;
    for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
        if (error || ++visited > directory_limit) {
            return std::nullopt;
        }
        if (lower(iterator->path().filename().native()) == wanted &&
            iterator->is_regular_file(error)) {
            return iterator->path().native();
        }
        error.clear();
    }
    return std::nullopt;
}

core::Result<std::optional<Lyrics>> read_lyrics(const std::string& raw_path,
                                                const core::CancellationToken& cancellation) {
    const auto cancelled = [&raw_path] {
        return core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "lyrics read was cancelled",
            .context = {{.key = "path", .value = core::escape_raw_path(raw_path)}},
        };
    };
    std::vector<Lyrics> found;
    if (const auto sidecar = lyrics_sidecar_path(raw_path)) {
        if (const auto bytes = read_small_file(*sidecar)) {
            auto lyrics = parse_lyrics(as_utf8(*bytes));
            lyrics.source = "sidecar";
            found.push_back(std::move(lyrics));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled());
    }
    // Read-only, as the metadata reader opens files (ADR-0232).
    TagLib::FileStream stream{raw_path.c_str(), true};
    TagLib::FileRef reference{&stream, false};
    if (!reference.isNull() && reference.file() != nullptr && reference.file()->isValid()) {
        if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(reference.file());
            mpeg != nullptr && mpeg->hasID3v2Tag()) {
            for (auto* frame : mpeg->ID3v2Tag()->frameList("SYLT")) {
                const auto* sylt = dynamic_cast<TagLib::ID3v2::SynchronizedLyricsFrame*>(frame);
                if (sylt == nullptr || sylt->timestampFormat() !=
                                           TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds) {
                    continue;
                }
                Lyrics lyrics{.source = "sylt", .synced = true, .lines = {}};
                for (const auto& entry : sylt->synchedText()) {
                    lyrics.lines.push_back({.time_ms = static_cast<std::int64_t>(entry.time),
                                            .text = std::string{trimmed(entry.text.to8Bit(true))}});
                }
                std::ranges::stable_sort(lyrics.lines, {}, [](const LyricsLine& line) {
                    return *line.time_ms;
                });
                found.push_back(std::move(lyrics));
                break;
            }
        }
        // An ID3 USLT frame with a description reads as LYRICS:<description>.
        const auto properties = reference.file()->properties();
        std::vector<std::string> keys{"LYRICS"};
        for (const auto& [key, values] : properties) {
            if (const auto name = key.to8Bit(true); name.starts_with("LYRICS:")) {
                keys.push_back(name);
            }
        }
        keys.emplace_back("UNSYNCEDLYRICS");
        for (const auto& key : keys) {
            const auto values = properties.find(TagLib::String{key, TagLib::String::UTF8});
            if (values == properties.end() || values->second.isEmpty()) {
                continue;
            }
            auto lyrics = parse_lyrics(values->second.toString("\n").to8Bit(true));
            lyrics.source = "tag:" + key;
            found.push_back(std::move(lyrics));
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled());
    }
    std::erase_if(found, [](const Lyrics& lyrics) { return !has_lines(lyrics); });
    if (const auto synced = std::ranges::find_if(found, &Lyrics::synced); synced != found.end()) {
        return std::optional<Lyrics>{std::move(*synced)};
    }
    if (!found.empty()) {
        return std::optional<Lyrics>{std::move(found.front())};
    }
    return std::optional<Lyrics>{};
}

} // namespace trackknife::metadata
