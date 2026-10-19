// SPDX-License-Identifier: GPL-3.0-only

// ADR-0296: playlist files as an engine's playlist folder holds them --
// MPD's, other players', Melody's own -- read and written again alike.

#include "trackknife/lists/playlist_file.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace lists = trackknife::lists;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// What is "on disk" for these files.
const std::set<std::string> present{"/music/A/B/01.flac", "/music/A/B/02.flac", "/music/C/D/01.mp3",
                                    "/home/u/Playlists/near/x.ogg", "/other/E/01.flac"};
const std::vector<std::string> roots{"/music", "/other"};

[[nodiscard]] lists::PlaylistFile read(std::string_view bytes, const std::string& path) {
    auto file = lists::parse_playlist_file(bytes, path, roots, [](const std::string& candidate) {
        return present.contains(candidate);
    });
    CHECK(file.has_value());
    return file ? *file : lists::PlaylistFile{};
}

void mpdsFilesAreRelativeToTheMusicFolder() {
    // As MPD writes them: no header, each line under music_directory.
    const auto file = read("A/B/01.flac\nC/D/01.mp3\nE/01.flac\nA/B/gone.flac\n",
                           "/home/u/.mpd/playlists/mix.m3u");
    CHECK(file.entries.size() == 4U);
    CHECK(file.entries[0].raw_path == "/music/A/B/01.flac");
    CHECK(file.entries[1].raw_path == "/music/C/D/01.mp3");
    // In the second music folder.
    CHECK(file.entries[2].raw_path == "/other/E/01.flac");
    // Gone, placed as the rest are.
    CHECK(file.entries[3].raw_path == "/music/A/B/gone.flac");
    CHECK(file.style == lists::PlaylistPathStyle::root_relative);
    // Written again, it is still MPD's.
    const auto again = lists::render_playlist_file(file.entries, "/home/u/.mpd/playlists/mix.m3u",
                                                   file.style, roots, {});
    CHECK(again == "#EXTM3U\nA/B/01.flac\nC/D/01.mp3\nE/01.flac\nA/B/gone.flac\n");
}

void otherPlayersAreRelativeToThePlaylist() {
    const auto file = read("#EXTM3U\r\n#EXTINF:215,A - One\r\n../../../music/A/B/01.flac\r\n"
                           "near/x.ogg\r\n# a comment\r\nhttp://radio.example/stream\r\n"
                           "C:\\Music\\x.mp3\r\n",
                           "/home/u/Playlists/mine.m3u8");
    CHECK(file.entries.size() == 2U);
    CHECK(file.skipped == 2U);
    CHECK(file.entries[0].raw_path == "/music/A/B/01.flac");
    CHECK(file.entries[0].title == "A - One");
    CHECK(file.entries[0].duration_ms == std::optional<std::int64_t>{215'000});
    CHECK(file.entries[1].raw_path == "/home/u/Playlists/near/x.ogg");
    CHECK(file.style == lists::PlaylistPathStyle::playlist_relative);
    const auto again = lists::render_playlist_file(file.entries, "/home/u/Playlists/mine.m3u8",
                                                   file.style, roots, {});
    CHECK(again == "#EXTM3U\n#EXTINF:215,A - One\n../../../music/A/B/01.flac\nnear/x.ogg\n");
}

void melodysOwnKeepWhatOnlyMelodyReads() {
    const lists::PlaylistFileEntry cue{.raw_path = "/music/A/B/01.flac",
                                       .title = "A - Two",
                                       .duration_ms = 180'400,
                                       .melody = R"({"segment":[0,100]})"};
    const auto written =
        lists::render_playlist_file(std::vector{cue}, "/music/Playlists/x.m3u8",
                                    lists::PlaylistPathStyle::root_relative, roots, "0a1b");
    CHECK(written == "#EXTM3U\n#MELODY-LIST:0a1b\n#MELODY:{\"segment\":[0,100]}\n"
                     "#EXTINF:180,A - Two\nA/B/01.flac\n");
    const auto file = read(written, "/music/Playlists/x.m3u8");
    CHECK(file.list_id == "0a1b");
    CHECK(file.entries.size() == 1U && file.entries[0].melody == R"({"segment":[0,100]})");
}

void absolutePathsStayWhole() {
    const auto file = read("/music/A/B/01.flac\nfile:///music/A/B/02.flac\n", "/tmp/x.m3u");
    CHECK(file.style == lists::PlaylistPathStyle::absolute);
    CHECK(file.entries.size() == 2U && file.entries[1].raw_path == "/music/A/B/02.flac");
    // A name a line cannot hold goes as a file URI.
    const auto odd = lists::render_playlist_file(
        std::vector{lists::PlaylistFileEntry{
            .raw_path = "/music/#1\nx.flac", .title = {}, .duration_ms = {}, .melody = {}}},
        "/tmp/x.m3u", lists::PlaylistPathStyle::absolute, roots, {});
    CHECK(odd == "#EXTM3U\nfile:///music/%231%0Ax.flac\n");
    // Outside every music folder, root-relative names it whole.
    const auto outside = lists::render_playlist_file(
        std::vector{lists::PlaylistFileEntry{
            .raw_path = "/elsewhere/y.flac", .title = {}, .duration_ms = {}, .melody = {}}},
        "/tmp/x.m3u", lists::PlaylistPathStyle::root_relative, roots, {});
    CHECK(outside == "#EXTM3U\n/elsewhere/y.flac\n");
}

void plsIsReadInItsNumbersOrder() {
    const auto file = read("[playlist]\nFile2=C/D/01.mp3\nTitle2=Two\nFile1=/music/A/B/01.flac\n"
                           "Length1=61\nNumberOfEntries=2\nVersion=2\n",
                           "/home/u/Playlists/old.pls");
    CHECK(file.entries.size() == 2U);
    CHECK(file.entries[0].raw_path == "/music/A/B/01.flac");
    CHECK(file.entries[0].duration_ms == std::optional<std::int64_t>{61'000});
    CHECK(file.entries[1].raw_path == "/music/C/D/01.mp3" && file.entries[1].title == "Two");
    const auto again = lists::render_playlist_file(file.entries, "/home/u/Playlists/old.pls",
                                                   lists::PlaylistPathStyle::absolute, roots, {});
    CHECK(again == "[playlist]\nFile1=/music/A/B/01.flac\nLength1=61\nFile2=/music/C/D/01.mp3\n"
                   "Title2=Two\nLength2=-1\nNumberOfEntries=2\nVersion=2\n");
}

void latin1TitlesAndBytesInNames() {
    // An old .m3u: a Latin-1 title, and a name in bytes kept as they are.
    const auto file = read("#EXTINF:1,Caf\xe9\n/music/caf\xe9.flac\n", "/tmp/old.m3u");
    CHECK(file.entries.size() == 1U);
    CHECK(file.entries[0].title == "Caf\xc3\xa9");
    CHECK(file.entries[0].raw_path == "/music/caf\xe9.flac");
    CHECK(lists::playlist_format("X.M3U8") == lists::PlaylistFormat::m3u);
    CHECK(lists::playlist_format("x.pls") == lists::PlaylistFormat::pls);
    CHECK(!lists::playlist_format("x.txt").has_value());
}

} // namespace

int main() {
    mpdsFilesAreRelativeToTheMusicFolder();
    otherPlayersAreRelativeToThePlaylist();
    melodysOwnKeepWhatOnlyMelodyReads();
    absolutePathsStayWhole();
    plsIsReadInItsNumbersOrder();
    latin1TitlesAndBytesInNames();
    if (failures != 0) {
        return EXIT_FAILURE;
    }
    std::cout << "playlist files: 6 scenarios\n";
    return EXIT_SUCCESS;
}
