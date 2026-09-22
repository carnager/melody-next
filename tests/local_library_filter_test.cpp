// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/persistence/rating_identity.hpp"
#include "trackknife/query/tkq.hpp"

#include <taglib/flacfile.h>
#include <taglib/tpropertymap.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    if (!input) {
        return std::nullopt;
    }
    std::string encoded{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    unsigned buffer = 0U;
    int bits = 0;
    for (const auto character : encoded) {
        if (character == '\n' || character == '\r' || character == '=') {
            continue;
        }
        const auto position = alphabet.find(character);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6U) | static_cast<unsigned>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFFU));
        }
    }
    return bytes;
}

std::string fixture(const std::filesystem::path& fixtures, const std::filesystem::path& root,
                    const std::string& name, const std::map<std::string, std::string>& tags) {
    std::filesystem::create_directories(root);
    const auto bytes = decode_base64_file(fixtures / "tagged-tone-flac.b64");
    if (!bytes) {
        return {};
    }
    const auto path = (root / name).native();
    {
        std::ofstream output{std::filesystem::path{path}, std::ios::binary};
        output.write(reinterpret_cast<const char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size()));
    }
    TagLib::FLAC::File file{path.c_str()};
    auto properties = file.properties();
    // The stock fixture carries its own tags; the test controls them all.
    properties.clear();
    for (const auto& [tag, value] : tags) {
        properties.replace(TagLib::String{tag, TagLib::String::UTF8},
                           TagLib::String{value, TagLib::String::UTF8});
    }
    file.setProperties(properties);
    if (!file.save()) {
        return {};
    }
    return path;
}

} // namespace

int main(const int argc, char** argv) {
    using namespace trackknife;
    CHECK(argc == 2);
    if (argc != 2) {
        return 1;
    }
    const std::filesystem::path fixtures{argv[1]};
    const auto base = std::filesystem::temp_directory_path() /
                      ("trackknife-filter-" + trackknife::core::StableId::random().to_string());
    std::error_code fs_error;
    std::filesystem::create_directories(base, fs_error);
    const auto root = base / "music";

    const auto jazz = fixture(fixtures, root, "a.flac",
                              {{"TITLE", "Alpha"},
                               {"ARTIST", "Miles Davis"},
                               {"ALBUM", "Kind of Blue"},
                               {"GENRE", "Jazz"},
                               {"DATE", "1959"},
                               {"REPLAYGAIN_TRACK_GAIN", "-6.02 dB"},
                               {"MUSICBRAINZ_TRACKID", "11111111-1111-1111-1111-111111111111"}});
    const auto rock = fixture(fixtures, root, "b.flac",
                              {{"TITLE", "Beta"},
                               {"ARTIST", "Band"},
                               {"ALBUM", "Loud"},
                               {"GENRE", "Rock"},
                               {"DATE", "1994-06-01"}});
    const auto bare = fixture(fixtures, root, "c.flac",
                              {{"TITLE", "Gamma"}, {"ARTIST", "Someone"}, {"ALBUM", "Quiet"}});
    CHECK(!jazz.empty() && !rock.empty() && !bare.empty());

    auto library = persistence::LocalLibrary::open(base / "state.sqlite");
    CHECK(library.has_value());
    if (!library) {
        return 1;
    }
    CHECK(library->add_root(root.native()).has_value());
    persistence::LibraryScanProgress progress;
    CHECK(library->scan({}, progress).has_value());
    CHECK(progress.indexed.load() == 3U);

    const auto paths_of = [&](const std::string& source) {
        auto compiled = query::compile_tkq(source);
        CHECK(compiled.has_value());
        if (!compiled) {
            return std::vector<std::string>{};
        }
        auto result = library->filter_paths(*compiled);
        CHECK(result.has_value());
        return result ? *result : std::vector<std::string>{};
    };
    const auto contains = [](const std::vector<std::string>& paths, const std::string& path) {
        for (const auto& entry : paths) {
            if (entry == path) {
                return true;
            }
        }
        return false;
    };

    // Text, existence, numeric, date, and technical predicates over the
    // migration-30 substrate.
    CHECK(paths_of("HISTORY(playcount) EQUAL 0").size() == 3U);
    auto repository = persistence::ListRepository::open(base / "state.sqlite");
    CHECK(repository.has_value());
    persistence::ListItem listened;
    listened.source = persistence::ListSource::local;
    listened.source_reference = jazz;
    const auto observed = core::observe_local_source_revision(jazz);
    CHECK(observed.has_value());
    listened.source_revision = *observed;
    CHECK(repository->record_local_listen(listened, core::StableId::random(), 1'000).has_value());
    CHECK(paths_of("HISTORY(playcount) EQUAL 1") == std::vector{jazz});
    CHECK(paths_of("HISTORY(albumplaycount) EQUAL 0").size() == 2U);
    CHECK(paths_of("HISTORY(dayssinceplayed) GREATER 180") == std::vector{jazz});
    CHECK(paths_of("HISTORY(lastplayed) MISSING").size() == 2U);
    CHECK(paths_of("HISTORY(albumlastplayed) PRESENT") == std::vector{jazz});
    CHECK(paths_of("playcount MISSING").size() == 3U);
    CHECK(paths_of("genre IS jazz") == std::vector{jazz});
    CHECK(paths_of("genre IS JAZZ") == std::vector{jazz});
    CHECK(paths_of("genre HAS ja") == std::vector{jazz});
    CHECK(paths_of("replaygain_track_gain PRESENT") == std::vector{jazz});
    const auto no_mbid = paths_of("musicbrainz_trackid MISSING");
    CHECK(no_mbid.size() == 2U && contains(no_mbid, rock) && contains(no_mbid, bare));
    CHECK(paths_of("date GREATER 1990") == std::vector{rock});
    CHECK(paths_of("date LESS 1960") == std::vector{jazz});
    CHECK(paths_of("codec IS flac AND samplerate GREATER 8000").size() == 3U);
    CHECK(paths_of("bitspersample EQUAL 16").size() == 3U);
    CHECK(paths_of("length_ms GREATER 0").size() == 3U);
    const auto either = paths_of("genre IS jazz OR genre IS rock");
    CHECK(either.size() == 2U && contains(either, jazz) && contains(either, rock));
    const auto not_rock = paths_of("NOT genre IS rock");
    CHECK(not_rock.size() == 2U && contains(not_rock, jazz) && contains(not_rock, bare));
    CHECK(paths_of("* HAS miles") == std::vector{jazz});
    CHECK(paths_of("miles blue") == std::vector{jazz});
    CHECK(paths_of("ALL").size() == 3U);

    // tkfmt expression predicates evaluate per candidate row, composing
    // with pushed conjuncts.
    CHECK(paths_of("\"%genre%\" IS jazz") == std::vector{jazz});
    CHECK(paths_of("\"%genre%\" MISSING") == std::vector{bare});
    CHECK(paths_of("date GREATER 1900 AND \"%genre%\" IS rock") == std::vector{rock});
    CHECK(paths_of("\"%genre%\" IS jazz OR genre IS rock").size() == 2U);

    // The sort clause orders the materialized match set; direction and the
    // stable default-order tiebreaker hold.
    {
        auto compiled = query::compile_tkq("ALL SORT DESCENDING BY %title%");
        CHECK(compiled.has_value());
        const auto page = library->filter(*compiled, 0U, 200U);
        CHECK(page.has_value());
        CHECK(page && page->entries.size() == 3U);
        CHECK(page && page->entries[0].key == bare && page->entries[1].key == rock &&
              page->entries[2].key == jazz);
        CHECK(page && !page->more);
    }

    // Paging and the more flag, on both the SQL fast path and the
    // materializing path.
    {
        auto compiled = query::compile_tkq("ALL");
        CHECK(compiled.has_value());
        const auto first = library->filter(*compiled, 0U, 2U);
        CHECK(first && first->entries.size() == 2U && first->more);
        const auto rest = library->filter(*compiled, 2U, 2U);
        CHECK(rest && rest->entries.size() == 1U && !rest->more);
        auto sorted = query::compile_tkq("ALL SORT BY %title%");
        CHECK(sorted.has_value());
        const auto page = library->filter(*sorted, 1U, 1U);
        CHECK(page && page->entries.size() == 1U && page->more);
        CHECK(page && page->entries.front().key == rock);
    }

    // Cancellation fails closed.
    {
        auto compiled = query::compile_tkq("ALL");
        core::CancellationSource cancel;
        cancel.request_cancellation();
        const auto cancelled = library->filter(*compiled, 0U, 200U, cancel.token());
        CHECK(!cancelled.has_value());
        CHECK(!cancelled && cancelled.error().code == core::ErrorCode::cancelled);
    }

    // Album predicates aggregate before ordinary predicates narrow candidates.
    const auto companion = fixture(fixtures, root, "d.flac",
                                   {{"TITLE", "Companion"},
                                    {"ARTIST", "Miles Davis"},
                                    {"ALBUM", "Kind of Blue"},
                                    {"DATE", "1959"}});
    persistence::LibraryScanProgress updated;
    CHECK(library->scan({}, updated).has_value());
    CHECK(paths_of("title IS Companion AND HISTORY(playcount) EQUAL 0") == std::vector{companion});
    CHECK(paths_of("title IS Companion AND HISTORY(albumplaycount) EQUAL 0").empty());
    CHECK(paths_of("title IS Companion AND HISTORY(albumplaycount) EQUAL 1") ==
          std::vector{companion});
    CHECK(paths_of("HISTORY(dayssinceplayed) GREATER -2") == std::vector{jazz});
    CHECK(paths_of("ALL SORT DESCENDING HISTORY(playcount)").front() == jazz);
    CHECK(paths_of("ALL SORT HISTORY(lastplayed)").back() == jazz);
    CHECK(paths_of("ALL SORT DESCENDING HISTORY(albumplaycount)").size() == 4U);
    const auto album_hash = persistence::album_rating_hash("Miles Davis", "Kind of Blue", "1959");
    auto companion_source = listened;
    companion_source.source_reference = companion;
    companion_source.source_revision = *core::observe_local_source_revision(companion);
    const auto whole_album = library->history_facts({{companion_source, album_hash},
                                                     {companion_source, album_hash},
                                                     {listened, album_hash},
                                                     {listened, album_hash}});
    CHECK(whole_album && (*whole_album)[0][0] == 0 && (*whole_album)[0][3] == 1 &&
          (*whole_album)[3][3] == 1);
    // Tab history comes from a consistent snapshot and does not count duplicate occurrences twice.
    const auto indexed_tracks = library->history_facts({{listened, ""}, {listened, ""}});
    CHECK(indexed_tracks && indexed_tracks->size() == 2U);
    CHECK(indexed_tracks && (*indexed_tracks)[0][0] == 1 && (*indexed_tracks)[1][3] == 1);
    auto logical = listened;
    logical.source_selection = persistence::ListItemSourceSelection{std::nullopt, 1};
    CHECK(repository->record_local_listen(logical, core::StableId::random(), 2000).has_value());
    const auto logical_history = library->history_facts({{logical, ""}, {listened, ""}});
    CHECK(logical_history && (*logical_history)[0][1] == 2000 && (*logical_history)[1][1] == 1000);
    auto missing_revision = listened;
    missing_revision.source_revision.reset();
    CHECK(!library->history_facts({{missing_revision, ""}}));
    core::CancellationSource stopped;
    stopped.request_cancellation();
    CHECK(!library->history_facts({{listened, ""}}, stopped.token()));
    const auto numeric_sort = query::compile_tkq("ALL SORT HISTORY(playcount)");
    persistence::TkqRowFacts two, ten;
    two.history = std::array<std::int64_t, 6>{2, -1, -1, 2, -1, -1};
    ten.history = std::array<std::int64_t, 6>{10, -1, -1, 10, -1, -1};
    CHECK(*persistence::tkq_sort_key(*numeric_sort, two) <
          *persistence::tkq_sort_key(*numeric_sort, ten));
    std::filesystem::remove_all(base, fs_error);
    return failures == 0 ? 0 : 1;
}
