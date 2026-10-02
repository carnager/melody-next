// SPDX-License-Identifier: GPL-3.0-only

// ADR-0254: a library view groups real, tagged files by its levels'
// expressions -- in the engine, the same answered locally and over the wire.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/remote_catalogue.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/client.hpp"
#include "trackknife/protocol/message.hpp"

#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace engine = trackknife::engine;
namespace persistence = trackknife::persistence;
namespace protocol = trackknife::protocol;
using persistence::LibraryEntryKind;
using persistence::LibraryQuery;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

struct Tags {
    std::string albumartist;
    std::string album;
    std::string date;
    std::vector<std::string> genres;
    std::string track;
};

[[nodiscard]] std::string tagged(const std::filesystem::path& fixtures,
                                 const std::filesystem::path& folder, const std::string& name,
                                 const Tags& tags) {
    std::ifstream input{fixtures / "tagged-tone-flac.b64"};
    std::string base64((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::erase(base64, '\n');
    const auto decoded = protocol::decode_raw_path(base64);
    require(decoded.has_value(), "the fixture decodes");
    std::filesystem::create_directories(folder);
    const auto path = (folder / name).string();
    {
        std::ofstream output{path, std::ios::binary};
        output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    }
    TagLib::FLAC::File file{path.c_str()};
    auto properties = file.properties();
    properties.replace("TITLE", TagLib::String{name, TagLib::String::UTF8});
    properties.replace("ARTIST", TagLib::String{tags.albumartist, TagLib::String::UTF8});
    properties.replace("ALBUMARTIST", TagLib::String{tags.albumartist, TagLib::String::UTF8});
    properties.replace("ALBUM", TagLib::String{tags.album, TagLib::String::UTF8});
    properties.replace("DATE", TagLib::String{tags.date, TagLib::String::UTF8});
    properties.replace("TRACKNUMBER", TagLib::String{tags.track, TagLib::String::UTF8});
    properties.erase("GENRE");
    if (!tags.genres.empty()) {
        TagLib::StringList genres;
        for (const auto& genre : tags.genres) {
            genres.append(TagLib::String{genre, TagLib::String::UTF8});
        }
        properties.replace("GENRE", genres);
    }
    file.setProperties(properties);
    require(file.save(), "the fixture is tagged");
    return path;
}

[[nodiscard]] LibraryQuery view(std::vector<persistence::LibraryViewLevel> levels,
                                std::vector<std::string> path = {}, std::string filter = {}) {
    LibraryQuery query;
    query.kind = LibraryEntryKind::group;
    query.view = std::move(levels);
    query.view_path = std::move(path);
    query.view_filter = std::move(filter);
    return query;
}

[[nodiscard]] std::vector<std::string> labels(const persistence::LibraryPage& page) {
    std::vector<std::string> found;
    for (const auto& entry : page.entries) {
        found.push_back(entry.view_value.empty() ? entry.label : entry.view_value);
    }
    return found;
}

[[nodiscard]] std::vector<std::string> names(const std::vector<std::string>& paths) {
    std::vector<std::string> found;
    for (const auto& path : paths) {
        found.push_back(std::filesystem::path{path}.filename().string());
    }
    return found;
}

void a_view_groups(const engine::Catalogue& catalogue, const std::string_view side) {
    const auto say = [side](const std::string& what) { return std::string{side} + ": " + what; };
    const std::vector<persistence::LibraryViewLevel> genres{
        {.format = "$each(genre)", .sort = {}, .descending = false},
        {.format = "%albumartist%", .sort = {}, .descending = false},
        {.format = "%album%", .sort = {}, .descending = false}};

    // $each puts a track under each of its genres; a track without one
    // goes under an empty label, last.
    auto top = catalogue.query(view(genres));
    require(top.has_value(), say("the view is grouped"));
    require(labels(*top) == std::vector<std::string>{"Jazz", "Rock", ""},
            say("genres in order, the empty one last"));
    require(top->entries[0].kind == LibraryEntryKind::group && top->entries[0].tracks == 2U &&
                top->entries[0].albums == 2U,
            say("a genre counts its tracks and albums"));
    require(top->entries[1].tracks == 3U, say("a track with two genres is under both"));

    auto rock = catalogue.query(view(genres, {"Rock"}));
    require(rock.has_value() && labels(*rock) == std::vector<std::string>{"Alpha", "Beta"},
            say("the next level, below the chosen node"));
    require(rock->entries[0].artist == "Alpha", say("a group of one artist names them"));

    // At the last level, one album is that album: its key finds its cover.
    auto alpha = catalogue.query(view(genres, {"Rock", "Alpha"}));
    require(alpha.has_value() && alpha->entries.size() == 1U, say("Alpha's one album"));
    require(alpha->entries[0].kind == LibraryEntryKind::album &&
                alpha->entries[0].view_value == "One" && !alpha->entries[0].key.empty() &&
                alpha->entries[0].key != "One",
            say("an album node is an album entry, keyed by its album"));

    auto tracks = catalogue.query(view(genres, {"Rock", "Alpha", "One"}));
    require(tracks.has_value() && tracks->entries.size() == 2U &&
                tracks->entries[0].kind == LibraryEntryKind::track,
            say("under the last level, its tracks"));
    require(std::filesystem::path{tracks->entries[0].key}.filename() == "a-02.flac" &&
                std::filesystem::path{tracks->entries[1].key}.filename() == "a-10.flac",
            say("in album order"));
    require(tracks->entries[0].label == "02. a-02.flac",
            say("a track is labelled as the artist tree labels it"));

    auto paths = catalogue.paths(view(genres, {"Rock"}));
    require(paths.has_value() &&
                names(*paths) == std::vector<std::string>{"a-02.flac", "a-10.flac", "d.flac"},
            say("a node's files, in library order"));

    auto gone = catalogue.query(view(genres, {"Polka"}));
    require(gone.has_value() && gone->entries.empty(), say("a node that is gone is empty"));

    // Ordered by value, newest first; digit runs as numbers.
    auto years = catalogue.query(view({{.format = "%date%", .sort = {}, .descending = true}}));
    require(years.has_value() &&
                labels(*years) == std::vector<std::string>{"2010", "2005", "1999"},
            say("descending"));
    auto numbered =
        catalogue.query(view({{.format = "Track %tracknumber%", .sort = {}, .descending = false}}));
    require(numbered.has_value() && labels(*numbered).size() >= 2U &&
                labels(*numbered)[0] == "Track 1" && labels(*numbered)[1] == "Track 2" &&
                labels(*numbered).back() == "Track 10",
            say("Track 2 before Track 10"));
    // By another expression than the label: albums by year.
    auto albums =
        catalogue.query(view({{.format = "%album%", .sort = "%date%", .descending = false}}));
    require(albums.has_value() &&
                labels(*albums) == std::vector<std::string>{"One", "Three", "Ten", "Two"},
            say("a sort expression orders a level, the label breaking ties"));

    // A filter narrows the library before it is grouped.
    auto jazz = catalogue.query(view(genres, {}, "genre IS jazz"));
    require(jazz.has_value() && labels(*jazz) == std::vector<std::string>{"Jazz", "Rock"} &&
                jazz->entries[1].tracks == 1U,
            say("only what the filter keeps is grouped"));

    require(!catalogue.query(view({{.format = "$if(", .sort = {}, .descending = false}})),
            say("a level that does not compile is refused"));

    // Folders: the roots, then what is in each, as indexed -- here as there.
    LibraryQuery folders;
    folders.folders = true;
    auto roots = catalogue.query(folders);
    require(roots.has_value() && roots->entries.size() == 1U &&
                roots->entries[0].kind == LibraryEntryKind::group &&
                roots->entries[0].tracks == 5U,
            say("the library's root, with what it holds"));
    folders.folder = roots->entries[0].view_value;
    auto inside = catalogue.query(folders);
    std::vector<std::string> names_inside;
    for (const auto& entry : inside ? inside->entries : std::vector<persistence::LibraryEntry>{}) {
        names_inside.push_back(entry.label);
    }
    require(inside.has_value() &&
                names_inside == std::vector<std::string>{"one", "ten", "three", "two"},
            say("the folders in the root, by name"));
    require(inside->entries[0].kind == LibraryEntryKind::album &&
                inside->entries[0].tracks == 2U && !inside->entries[0].rating_hash.empty(),
            say("a folder of one album is that album"));
    folders.folder = inside->entries[0].view_value;
    auto one = catalogue.query(folders);
    require(one.has_value() && one->entries.size() == 2U &&
                one->entries[0].kind == LibraryEntryKind::track &&
                one->entries[0].label == "02. a-02.flac" &&
                one->entries[1].label == "10. a-10.flac",
            say("a folder's tracks, in track order"));
    folders.folder = roots->entries[0].view_value;
    auto everything = catalogue.paths(folders);
    require(everything.has_value() && everything->size() == 5U &&
                names(*everything)[0] == "a-02.flac" && names(*everything)[1] == "a-10.flac",
            say("a folder's files are everything below it"));
    require(!catalogue.query(view(genres, {}, "genre (((")), say("so is a bad filter"));
}

} // namespace

int main(const int argc, char** argv) {
    require(argc == 2, "the fixtures folder is given");
    const std::filesystem::path fixtures{argv[1]};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-views-" + core::StableId::random().to_string());
    const auto music = directory / "music";
    static_cast<void>(tagged(fixtures, music / "one", "a-02.flac",
                             {"Alpha", "One", "1999", {"Rock", "Jazz"}, "2"}));
    static_cast<void>(
        tagged(fixtures, music / "one", "a-10.flac", {"Alpha", "One", "1999", {"Rock"}, "10"}));
    static_cast<void>(
        tagged(fixtures, music / "two", "b.flac", {"Beta", "Two", "2010", {"Jazz"}, "1"}));
    static_cast<void>(tagged(fixtures, music / "three", "c.flac", {"Gamma", "Three", "2005", {}, "1"}));
    static_cast<void>(
        tagged(fixtures, music / "ten", "d.flac", {"Beta", "Ten", "2010", {"Rock"}, "1"}));

    engine::LocalCatalogue local{directory / "library.sqlite3"};
    require(local.prepare().has_value(), "the catalogue opens");
    require(local.add_root(music.string()).has_value(), "its folder is added");
    persistence::LibraryScanProgress progress;
    require(local.scan({}, progress).has_value(), "scanned");

    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, local);
    auto server = engine::Server::listen(directory / "engine.sock", dispatcher);
    require(server.has_value(), "the engine binds");
    (*server)->start();
    auto client = protocol::Client::connect(directory / "engine.sock");
    require(client.has_value(), "the client connects");
    engine::RemoteCatalogue remote{**client};

    a_view_groups(local, "local");
    a_view_groups(remote, "remote");

    // Kept, the tree follows what it read: a rating given to an album shows
    // on its node at once.
    const std::vector<persistence::LibraryViewLevel> artists{
        {.format = "%albumartist%", .sort = {}, .descending = false},
        {.format = "%album%", .sort = {}, .descending = false}};
    auto before = local.query(view(artists, {"Alpha"}));
    require(before.has_value() && before->entries.size() == 1U &&
                before->entries[0].rating == 0U,
            "the album is unrated");
    require(local.set_rating(before->entries[0].rating_hash, true, 8U).has_value(),
            "it is rated");
    auto after = local.query(view(artists, {"Alpha"}));
    require(after.has_value() && after->entries[0].rating == 8U, "the view shows the rating");
    // A rating as a level reads the same.
    auto rated = local.query(view({{.format = "$if2(%albumrating%,unrated)", .sort = {},
                                    .descending = false}}));
    require(rated.has_value() && labels(*rated) == std::vector<std::string>{"8", "unrated"},
            "a level can group by rating");

    (*server)->stop();
    std::filesystem::remove_all(directory);
    return EXIT_SUCCESS;
}
