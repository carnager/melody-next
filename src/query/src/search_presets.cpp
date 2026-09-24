// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/query/search_presets.hpp"
#include "trackknife/query/tkq.hpp"

#include <charconv>

namespace trackknife::query {
namespace {
constexpr SearchPreset presets[]{
    {"year", "Explore", "Music from a year…", "date EQUAL {value}", PresetInput::integer, "Year",
     "1990", 1, 9999},
    {"decade", "Explore", "Music from a decade…",
     "date GREATER {previous} AND date LESS {nextdecade}", PresetInput::integer,
     "First year of the decade (e.g. 1990)", "1990", 10, 9990},
    {"artist", "Explore", "Artist…", "artist HAS {value}", PresetInput::text, "Artist contains",
     "Miles Davis"},
    {"album-artist", "Explore", "Album artist…", "albumartist HAS {value}", PresetInput::text,
     "Album artist contains", "Miles Davis"},
    {"genre", "Explore", "Genre…", "genre HAS {value}", PresetInput::text, "Genre contains",
     "Jazz"},
    {"album", "Explore", "Album title…", "album HAS {value}", PresetInput::text,
     "Album title contains", "Kind of Blue"},
    {"recently-added", "Explore", "Albums added in the last N days…",
     "albumdayssinceadded LESS {value}", PresetInput::integer,
     "Days since the album's newest track came into the library", "30", 1, 36500},
    {"track-rating", "Favourites", "Tracks rated at least…", "rating GREATER {previous}",
     PresetInput::integer, "Minimum rating (1–10)", "8", 1, 10},
    {"album-rating", "Favourites", "Albums rated at least…", "albumrating GREATER {previous}",
     PresetInput::integer, "Minimum album rating (1–10)", "8", 1, 10},
    {"unrated-tracks", "Favourites", "Unrated tracks", "rating MISSING"},
    {"unrated-albums", "Favourites", "Unrated albums", "albumrating MISSING"},
    {"unplayed-tracks", "Listening", "Never-played tracks", "HISTORY(playcount) EQUAL 0"},
    {"unplayed-albums", "Listening", "Never-played albums", "HISTORY(albumplaycount) EQUAL 0"},
    {"recently-played", "Listening", "Played in the last N days…",
     "HISTORY(dayssinceplayed) LESS {value}", PresetInput::integer,
     "Days (complete 24-hour periods)", "30", 1, 36500},
    {"forgotten-albums", "Listening", "Albums not played for N days…",
     "HISTORY(albumplaycount) EQUAL 0 OR HISTORY(albumdayssinceplayed) GREATER {previous}",
     PresetInput::integer, "Days (includes never-played albums)", "180", 1, 36500},
    {"most-played", "Listening", "Most-played tracks first",
     "HISTORY(playcount) GREATER 0 SORT DESCENDING HISTORY(playcount)"},
    {"codec", "Audio properties", "Codec…", "codec IS {value}", PresetInput::text,
     "Codec name (e.g. flac, mp3, opus)", "flac"},
    {"sample-rate", "Audio properties", "Sample rate…", "samplerate EQUAL {value}",
     PresetInput::integer, "Sample rate in Hz", "96000", 1, 1536000},
    {"bit-depth", "Audio properties", "Bit depth…", "bitspersample EQUAL {value}",
     PresetInput::integer, "Bits per sample", "24", 1, 64},
    {"multichannel", "Audio properties", "Multichannel audio", "channels GREATER 2"},
    {"missing-album-artist", "Library maintenance", "Missing album artist", "albumartist MISSING"},
    {"missing-year", "Library maintenance", "Missing release date", "date MISSING"},
    {"missing-genre", "Library maintenance", "Missing genre", "genre MISSING"},
    {"missing-album", "Library maintenance", "Missing album title", "album MISSING"},
    {"missing-track-gain", "Library maintenance", "Missing track ReplayGain tag",
     "REPLAYGAIN_TRACK_GAIN MISSING"},
    {"missing-album-gain", "Library maintenance", "Missing album ReplayGain tag",
     "REPLAYGAIN_ALBUM_GAIN MISSING"},
};

core::Result<std::string> invalid(std::string message) {
    return std::unexpected(core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}});
}

void replace(std::string& text, std::string_view token, const std::string& value) {
    std::size_t offset = 0;
    while ((offset = text.find(token, offset)) != std::string::npos) {
        text.replace(offset, token.size(), value);
        offset += value.size();
    }
}
} // namespace

std::span<const SearchPreset> search_presets() { return presets; }

core::Result<std::string> preset_query(const SearchPreset& preset, std::string_view value) {
    std::string result{preset.pattern};
    if (preset.input == PresetInput::integer) {
        if (value.empty())
            return invalid("Enter a whole number within the preset's range");
        int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (value.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() || number < preset.minimum ||
            number > preset.maximum)
            return invalid("Enter a whole number within the preset's range");
        if (preset.id == "decade" && number % 10 != 0)
            return invalid("A decade must start with a year ending in zero");
        replace(result, "{value}", std::to_string(number));
        replace(result, "{previous}", std::to_string(number - 1));
        replace(result, "{nextdecade}", std::to_string(number + 10));
    } else if (preset.input == PresetInput::text) {
        if (value.empty() || value.size() > 1024 ||
            value.find_first_of("\r\n") != std::string_view::npos ||
            value.find('\0') != std::string_view::npos)
            return invalid("Enter non-empty text up to 1024 bytes on one line");
        std::string quoted{"\""};
        for (const char character : value) {
            quoted += character;
            if (character == '"')
                quoted += '"';
        }
        quoted += '"';
        replace(result, "{value}", quoted);
    }
    const auto compiled = compile_tkq(result);
    if (!compiled)
        return std::unexpected(compiled.error());
    return result;
}
} // namespace trackknife::query
