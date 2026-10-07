// SPDX-License-Identifier: GPL-3.0-only

// ADR-0254: how long grouping a large library into a view takes, and how
// long opening a node of a kept view takes. One real file is scanned and its
// row cloned into a synthetic library of 2,000 albums by 400 artists, each
// track with two genres.
//
//   trackknife_library_views_benchmark <fixtures> [--quick]

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/message.hpp"

#include <sqlite3.h>

#include <chrono>
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

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void execute(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << sql << ": " << (error != nullptr ? error : "?") << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Every column of the scanned row but those named here, which each clone
// gives its own values.
[[nodiscard]] std::vector<std::string> other_columns(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA table_info(local_library_tracks)", -1, &statement, nullptr);
    std::vector<std::string> columns;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        std::string name{reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))};
        if (name != "raw_path" && name != "album_key" && name != "album" && name != "artist" &&
            name != "track" && name != "title") {
            columns.push_back(std::move(name));
        }
    }
    sqlite3_finalize(statement);
    return columns;
}

void clone(const std::filesystem::path& database, const std::size_t tracks) {
    sqlite3* db = nullptr;
    require(sqlite3_open(database.c_str(), &db) == SQLITE_OK, "the database opens");
    std::string list;
    for (const auto& column : other_columns(db)) {
        list += "," + column;
    }
    execute(db, "BEGIN");
    execute(db, "CREATE TEMP TABLE seed AS SELECT * FROM local_library_tracks LIMIT 1");
    const auto insert = "INSERT INTO local_library_tracks(raw_path,album_key,album,artist,track,"
                        "title" +
                        list + ") SELECT ?1,?2,?3,?4,?5,?6" + list + " FROM seed";
    sqlite3_stmt* row = nullptr;
    require(sqlite3_prepare_v2(db, insert.c_str(), -1, &row, nullptr) == SQLITE_OK,
            "the clone statement prepares");
    sqlite3_stmt* field = nullptr;
    require(sqlite3_prepare_v2(db,
                               "INSERT INTO local_library_fields(raw_path,canonical_name,position,"
                               "value,value_lower) VALUES(?1,?2,?3,?4,lower(?4))",
                               -1, &field, nullptr) == SQLITE_OK,
            "the field statement prepares");
    const auto put_field = [&](const std::string& path, const char* name, const int position,
                               const std::string& value) {
        sqlite3_reset(field);
        sqlite3_bind_blob(field, 1, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(field, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(field, 3, position);
        sqlite3_bind_blob(field, 4, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        require(sqlite3_step(field) == SQLITE_DONE, "a field is cloned");
    };
    for (std::size_t index = 0U; index < tracks; ++index) {
        const auto album = index / 10U;
        const auto artist = "Artist " + std::to_string(album % 400U);
        const auto album_name = "Album " + std::to_string(album);
        const auto path =
            "/synthetic/" + std::to_string(album) + "/" + std::to_string(index) + ".flac";
        const auto key = "album:" + std::to_string(album);
        const auto title = "Track " + std::to_string(index);
        sqlite3_reset(row);
        sqlite3_bind_blob(row, 1, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(row, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(row, 3, album_name.data(), static_cast<int>(album_name.size()),
                          SQLITE_TRANSIENT);
        sqlite3_bind_blob(row, 4, artist.data(), static_cast<int>(artist.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(row, 5, static_cast<sqlite3_int64>(index % 10U + 1U));
        sqlite3_bind_blob(row, 6, title.data(), static_cast<int>(title.size()), SQLITE_TRANSIENT);
        require(sqlite3_step(row) == SQLITE_DONE, "a track is cloned");
        put_field(path, "albumartist", 0, artist);
        put_field(path, "artist", 0, artist);
        put_field(path, "album", 0, album_name);
        put_field(path, "title", 0, title);
        put_field(path, "date", 0, std::to_string(1960U + album % 60U));
        put_field(path, "genre", 0, "Genre " + std::to_string(album % 25U));
        put_field(path, "genre", 1, "Genre " + std::to_string((album + 7U) % 25U));
        put_field(path, "tracknumber", 0, std::to_string(index % 10U + 1U));
    }
    sqlite3_finalize(row);
    sqlite3_finalize(field);
    execute(db, "COMMIT");
    sqlite3_close(db);
}

template <typename F> double seconds(F&& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(const int argc, char** argv) {
    require(argc >= 2, "the fixtures folder is given");
    const bool quick = argc > 2 && std::string_view{argv[2]} == "--quick";
    const std::size_t tracks = quick ? 2'000U : 100'000U;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-views-bench-" + core::StableId::random().to_string());
    const auto music = directory / "music";
    std::filesystem::create_directories(music);
    {
        std::ifstream input{std::filesystem::path{argv[1]} / "tagged-tone-flac.b64"};
        std::string base64((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        std::erase(base64, '\n');
        const auto decoded = trackknife::protocol::decode_raw_path(base64);
        require(decoded.has_value(), "the fixture decodes");
        std::ofstream output{music / "seed.flac", std::ios::binary};
        output.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    }
    const auto database = directory / "library.sqlite3";
    engine::LocalCatalogue catalogue{database};
    require(catalogue.prepare().has_value(), "the catalogue opens");
    require(catalogue.add_root(music.string()).has_value(), "the folder is added");
    persistence::LibraryScanProgress progress;
    require(catalogue.scan({}, progress).has_value(), "the seed is scanned");
    clone(database, tracks);

    persistence::LibraryQuery query;
    query.kind = persistence::LibraryEntryKind::group;
    query.view = {{.format = "$each(genre)", .sort = {}, .descending = false},
                  {.format = "%albumartist%", .sort = {}, .descending = false},
                  {.format = "%album%", .sort = "%date%", .descending = false}};
    std::size_t groups = 0U;
    const auto built = seconds([&] {
        auto page = catalogue.query(query);
        require(page.has_value(), "the view is grouped");
        groups = page->entries.size();
    });
    query.view_path = {"Genre 3"};
    const auto opened = seconds([&] {
        auto page = catalogue.query(query);
        require(page.has_value() && !page->entries.empty(), "a kept node opens");
    });
    std::cout << tracks + 1U << " tracks, " << groups << " genres: grouped in " << built
              << " s; a node of the kept view opened in " << opened * 1'000.0 << " ms\n";
    std::filesystem::remove_all(directory);
    return EXIT_SUCCESS;
}
