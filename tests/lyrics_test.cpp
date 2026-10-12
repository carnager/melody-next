// SPDX-License-Identifier: GPL-3.0-only

// ADR-0276: lyrics, parsed and found. The parser cases are the corpus; each
// container is a real file, lyrics written into it by TagLib as a tagger
// would, then found by read_lyrics.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/lyrics.hpp"

#include <fileref.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <synchronizedlyricsframe.h>
#include <tpropertymap.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace metadata = trackknife::metadata;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] metadata::LyricsLine at(const std::int64_t ms, std::string text) {
    return {.time_ms = ms, .text = std::move(text)};
}

[[nodiscard]] metadata::LyricsLine plain(std::string text) {
    return {.time_ms = std::nullopt, .text = std::move(text)};
}

void parsing() {
    // Plain text stays line by line; blank lines inside are verses apart,
    // blank lines around are dropped. A section marker is text.
    const auto prose = metadata::parse_lyrics("\n\nFirst line\r\n\r\n[Chorus]\nSecond\n\n");
    CHECK(!prose.synced);
    CHECK((prose.lines ==
           std::vector{plain("First line"), plain(""), plain("[Chorus]"), plain("Second")}));

    // Synced, with every timing form, a header, a BOM and CRLF.
    const auto lrc =
        metadata::parse_lyrics("\xEF\xBB\xBF[ar:Someone]\r\n[ti:Song]\r\n"
                               "[00:01.5]One\r\n[00:02.25]Two\r\n"
                               "[00:03.125]Three\r\n[00:04:50]Four\r\n[01:05]Five\r\n");
    CHECK(lrc.synced);
    CHECK((lrc.lines == std::vector{at(1'500, "One"), at(2'250, "Two"), at(3'125, "Three"),
                                    at(4'500, "Four"), at(65'000, "Five")}));

    // Several timings on a line are that line at each time, in time order.
    const auto repeated = metadata::parse_lyrics("[00:10.00][00:30.00]Chorus\n[00:20.00]Verse");
    CHECK((repeated.lines ==
           std::vector{at(10'000, "Chorus"), at(20'000, "Verse"), at(30'000, "Chorus")}));

    // A positive offset shows the lines sooner, never before the start.
    const auto offset = metadata::parse_lyrics("[offset:+500]\n[00:00.20]Early\n[00:02.00]Late");
    CHECK((offset.lines == std::vector{at(0, "Early"), at(1'500, "Late")}));
    const auto later = metadata::parse_lyrics("[offset:-250]\n[00:01.00]Late");
    CHECK((later.lines == std::vector{at(1'250, "Late")}));

    // Word timings go; an empty timed line stays, a pause in the singing.
    const auto words =
        metadata::parse_lyrics("[00:01.00]<00:01.00>Hello <00:01.50>world\n[00:03.00]\n");
    CHECK((words.lines == std::vector{at(1'000, "Hello world"), at(3'000, "")}));

    // In synced text, an untimed line is left out; a bracket that is not a
    // timing stays text; a malformed timing makes the line plain.
    const auto mixed = metadata::parse_lyrics("Credits\n[00:01.00]A <b> c\n[0x:01]Odd");
    CHECK(mixed.synced);
    CHECK((mixed.lines == std::vector{at(1'000, "A <b> c")}));
    CHECK(!metadata::parse_lyrics("[00:61.00]Not a second").synced);
    CHECK(metadata::parse_lyrics("").lines.empty());

    // [length:] is kept, to tell versions of the song apart.
    CHECK(metadata::parse_lyrics("[length: 05:52]\n[00:01.00]A").declared_length_ms ==
          std::optional<std::int64_t>{352'000});
    CHECK(!metadata::parse_lyrics("[00:01.00]A").declared_length_ms);
}

[[nodiscard]] std::string decoded(const std::filesystem::path& fixture) {
    std::ifstream input{fixture, std::ios::binary};
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string bytes;
    unsigned accumulator = 0U;
    unsigned bits = 0U;
    for (const char character : encoded) {
        const auto value = alphabet.find(character);
        if (character == '=') {
            break;
        }
        if (value == std::string_view::npos) {
            continue;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            bytes.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    return bytes;
}

[[nodiscard]] std::filesystem::path materialized(const std::filesystem::path& fixtures,
                                                 const std::filesystem::path& directory,
                                                 const std::string& fixture,
                                                 const std::string& name) {
    const auto path = directory / name;
    std::ofstream output{path, std::ios::binary};
    const auto bytes = decoded(fixtures / fixture);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

void tag(const std::filesystem::path& path, const char* key, const std::string& text) {
    TagLib::FileRef file{path.c_str()};
    CHECK(!file.isNull());
    auto properties = file.file()->properties();
    properties.replace(key, TagLib::StringList{TagLib::String{text, TagLib::String::UTF8}});
    file.file()->setProperties(properties);
    CHECK(file.save());
}

void finding(const std::filesystem::path& fixtures, const std::filesystem::path& directory) {
    // Every container that carries a LYRICS tag gives it back.
    constexpr std::array containers{
        std::pair{"tagged-tone-flac.b64", ".flac"},  std::pair{"tagged-tone-mp3.b64", ".mp3"},
        std::pair{"tagged-tone-m4a.b64", ".m4a"},    std::pair{"tagged-tone-opus.b64", ".opus"},
        std::pair{"tagged-tone-vorbis.b64", ".ogg"}, std::pair{"tagged-tone-wavpack.b64", ".wv"}};
    for (const auto& [fixture, suffix] : containers) {
        const auto path = materialized(fixtures, directory, fixture, std::string{"song"} + suffix);
        const auto none = metadata::read_lyrics(path.string());
        CHECK(none.has_value() && !none->has_value());
        tag(path, "LYRICS", "Line one\nLine two");
        const auto found = metadata::read_lyrics(path.string());
        CHECK(found.has_value() && found->has_value());
        if (found && *found) {
            CHECK((*found)->source == "tag:LYRICS");
            CHECK(!(*found)->synced);
            CHECK(((*found)->lines == std::vector{plain("Line one"), plain("Line two")}));
        }
        std::filesystem::remove(path);
    }

    // Plain lyrics embedded, synced ones fetched later beside the file:
    // synced wins, wherever it comes from. The sidecar's name is matched
    // without regard to case, and Latin-1 is read as such.
    const auto flac = materialized(fixtures, directory, "tagged-tone-flac.b64", "Track.flac");
    tag(flac, "LYRICS", "Plain words");
    {
        std::ofstream sidecar{directory / "TRACK.LRC", std::ios::binary};
        sidecar << "[00:01.00]Caf\xE9\n";
    }
    const auto both = metadata::read_lyrics(flac.string());
    CHECK(both && *both && (*both)->source == "sidecar" && (*both)->synced);
    if (both && *both) {
        CHECK(((*both)->lines == std::vector{at(1'000, "Caf\xC3\xA9")}));
    }
    // A plain sidecar loses to synced lyrics in the tag.
    {
        std::ofstream sidecar{directory / "TRACK.LRC", std::ios::binary | std::ios::trunc};
        sidecar << "Plain sidecar\n";
    }
    tag(flac, "LYRICS", "[00:02.00]Synced tag");
    const auto tagged = metadata::read_lyrics(flac.string());
    CHECK(tagged && *tagged && (*tagged)->source == "tag:LYRICS" && (*tagged)->synced);

    // Several LYRICS values are several versions, never one text: here two
    // copies of lyrics timed for a longer edit of the song, and one that
    // says nothing of its length. The copies are one version; the one that
    // may fit the track is chosen.
    std::filesystem::remove(directory / "TRACK.LRC");
    const auto versions = materialized(fixtures, directory, "tagged-tone-flac.b64", "multi.flac");
    {
        TagLib::FileRef file{versions.c_str()};
        auto properties = file.file()->properties();
        const TagLib::String other_edit{"[length: 05:52]\n[00:21.81]Life is a mystery\n"
                                        "[00:30.50]I hear you call my name",
                                        TagLib::String::UTF8};
        const TagLib::String this_edit{"[00:00.50]Life is a mystery\n[00:00.90]Everyone",
                                       TagLib::String::UTF8};
        properties.replace("LYRICS", TagLib::StringList{other_edit, other_edit, this_edit});
        file.file()->setProperties(properties);
        CHECK(file.save());
    }
    const auto chosen = metadata::read_lyrics(versions.string());
    CHECK(chosen && *chosen && (*chosen)->synced);
    if (chosen && *chosen) {
        CHECK(((*chosen)->lines == std::vector{at(500, "Life is a mystery"), at(900, "Everyone")}));
    }
    // Only versions for another edit: the first is still shown.
    {
        TagLib::FileRef file{versions.c_str()};
        auto properties = file.file()->properties();
        properties.replace("LYRICS",
                           TagLib::StringList{TagLib::String{"[length: 05:52]\n[00:21.81]Only"}});
        file.file()->setProperties(properties);
        CHECK(file.save());
    }
    const auto only = metadata::read_lyrics(versions.string());
    CHECK(only && *only && (*only)->lines.size() == 1U);

    // An MP3's SYLT frame, in milliseconds.
    const auto mp3 = materialized(fixtures, directory, "tagged-tone-mp3.b64", "synced.mp3");
    {
        TagLib::MPEG::File file{mp3.c_str()};
        auto* frame = new TagLib::ID3v2::SynchronizedLyricsFrame(TagLib::String::UTF8);
        frame->setTimestampFormat(TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds);
        frame->setSynchedText({{2'000U, "Second"}, {1'000U, "\nFirst"}});
        file.ID3v2Tag(true)->addFrame(frame);
        CHECK(file.save());
    }
    const auto sylt = metadata::read_lyrics(mp3.string());
    CHECK(sylt && *sylt && (*sylt)->source == "sylt");
    if (sylt && *sylt) {
        CHECK(((*sylt)->lines == std::vector{at(1'000, "First"), at(2'000, "Second")}));
    }

    // A file nothing can read may still have lyrics beside it.
    const auto opaque = directory / "opaque.xyz";
    std::ofstream{opaque} << "not audio";
    std::ofstream{directory / "opaque.lrc"} << "[00:00.50]Still";
    const auto beside = metadata::read_lyrics(opaque.string());
    CHECK(beside && *beside && (*beside)->source == "sidecar");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: lyrics_test <fixture-dir>\n";
        return 2;
    }
    parsing();
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("trackknife-lyrics-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    finding(argv[1], directory);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::cerr << failures << " lyrics checks failed\n";
        return 1;
    }
    std::cout << "lyrics: all checks passed\n";
    return 0;
}
