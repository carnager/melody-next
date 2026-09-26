// SPDX-License-Identifier: GPL-3.0-only

// ADR-0232: melody-watch's parts -- the folder mapping, the catch-up
// comparison, and watching a real tree with inotify.

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/watch/watch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace watch = trackknife::watch;
using Kind = watch::Change::Kind;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void a_folder_is_mapped_to_where_the_engine_sees_it() {
    const auto mapped = watch::parse_mapping("/volume1/music/=/mnt/nas/music");
    require(mapped && mapped->local == "/volume1/music" && mapped->engine == "/mnt/nas/music",
            "LOCAL=ENGINE, trailing slashes dropped");
    const auto same = watch::parse_mapping("/srv/music");
    require(same && same->local == "/srv/music" && same->engine == "/srv/music",
            "one path is the same on both machines");
    const auto odd = watch::parse_mapping("/srv/a=b/music=/mnt/a=b");
    require(odd && odd->local == "/srv/a=b/music" && odd->engine == "/mnt/a=b",
            "a path may contain '=': the split is at the first \"=/\"");
    require(!watch::parse_mapping("music"), "a relative folder is refused");
    require(!watch::parse_mapping("music=/srv/music"), "and so is one with a relative local side");
    const auto literal = watch::parse_mapping("/srv/music=music");
    require(literal && literal->local == "/srv/music=music" && literal->engine == literal->local,
            "with no \"=/\" the '=' is part of the folder's name");

    require(watch::to_engine(*mapped, "/volume1/music/A/01.flac") == "/mnt/nas/music/A/01.flac",
            "a file is named as the engine names it");
    require(!watch::to_engine(*mapped, "/volume1/musicals/01.flac"),
            "a folder whose name merely starts the same is not inside");
    require(watch::contains("/volume1/music", "/volume1/music"), "a folder contains itself");
    require(!watch::contains("/volume1/music", "/volume1/music0"), "not its neighbour");
    require(trackknife::core::is_audio_path("/x/Song.FLAC") &&
                !trackknife::core::is_audio_path("/x/cover.jpg") &&
                !trackknife::core::is_audio_path("/x/.flac"),
            "audio is told by extension, in any case, and a hidden name is not one");
}

void catching_up_finds_what_changed_while_nobody_watched() {
    const std::vector<watch::PresentFile> present{
        {.path = "/m/same.flac", .size = 10, .modified_seconds = 100},
        {.path = "/m/retagged.flac", .size = 11, .modified_seconds = 200},
        {.path = "/m/new.flac", .size = 12, .modified_seconds = 300},
        {.path = "/m/offline.flac", .size = 13, .modified_seconds = 400},
    };
    const std::vector<watch::IndexedFile> indexed{
        {.path = "/m/same.flac", .size = 10, .modified_seconds = 100, .available = true},
        {.path = "/m/retagged.flac", .size = 11, .modified_seconds = 150, .available = true},
        {.path = "/m/offline.flac", .size = 13, .modified_seconds = 400, .available = false},
        {.path = "/m/deleted.flac", .size = 14, .modified_seconds = 500, .available = true},
    };
    const auto plan = watch::plan_catch_up(present, indexed);
    require(plan.added == 1U && plan.changed == 2U && plan.vanished == 1U, "each kind is counted");
    require(plan.refresh == std::vector<std::string>{"/m/deleted.flac", "/m/new.flac",
                                                     "/m/offline.flac", "/m/retagged.flac"},
            "new, changed, unavailable and deleted files are re-read; the unchanged one is not");
    require(!plan.vanished_withheld, "one file gone is a file deleted");

    // Everything gone at once is a wrong mapping or a missing mount.
    std::vector<watch::IndexedFile> library;
    for (int index = 0; index < 40; ++index) {
        library.push_back({.path = "/m/" + std::to_string(index) + ".flac",
                           .size = 1,
                           .modified_seconds = 1,
                           .available = true});
    }
    const auto suspicious = watch::plan_catch_up({}, library);
    require(suspicious.vanished == 40U && suspicious.vanished_withheld &&
                suspicious.refresh.empty(),
            "most of the library gone is not passed on as deletions");
}

void write_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << "audio";
}

// Reads until `wanted` has been reported, or gives up after five seconds.
[[nodiscard]] bool reported(watch::TreeWatcher& watcher, const watch::Change& wanted,
                            std::vector<watch::Change>* seen = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& change : watcher.read(std::chrono::milliseconds{100})) {
            if (seen != nullptr) {
                seen->push_back(change);
            }
            if (change == wanted) {
                return true;
            }
        }
    }
    return false;
}

void a_tree_is_watched_as_it_changes(const std::filesystem::path& base) {
    const auto root = base / "music";
    const auto outside = base / "outside";
    std::filesystem::create_directories(root / "Existing");
    std::filesystem::create_directories(outside);
    auto made = watch::TreeWatcher::create();
    require(made.has_value(), "inotify is there");
    auto& watcher = **made;
    require(watcher.add_tree(root.native()).has_value(), "the tree is watched");
    require(watcher.watched() == 2U, "every folder in it");
    require(!watcher.add_tree((base / "absent").native()), "a folder that is not there is refused");

    const auto file = [](const std::filesystem::path& path) {
        return watch::Change{.kind = Kind::file, .path = path.native()};
    };

    write_file(root / "Existing" / "01.flac");
    require(reported(watcher, file(root / "Existing" / "01.flac")), "a file written is reported");

    // A folder made and filled at once: its files are found even if they
    // were written before its watch existed.
    write_file(root / "New" / "CD1" / "01.flac");
    require(reported(watcher, file(root / "New" / "CD1" / "01.flac")),
            "a new folder's files are reported");
    write_file(root / "New" / "CD1" / "02.flac");
    require(reported(watcher, file(root / "New" / "CD1" / "02.flac")),
            "and the new folder is watched from then on");

    // Moved out whole: reported as the folder, and no longer watched.
    std::filesystem::rename(root / "New", outside / "New");
    require(reported(watcher, {.kind = Kind::folder_gone, .path = (root / "New").native()}),
            "a folder moved out is reported as gone");
    std::vector<watch::Change> after;
    write_file(outside / "New" / "CD1" / "03.flac");
    static_cast<void>(reported(watcher, file(outside / "New" / "CD1" / "03.flac"), &after));
    require(std::ranges::none_of(after, [&](const auto& change) {
                return change.path.find("03.flac") != std::string::npos;
            }),
            "what happens to it outside is not the tree's business");

    // Moved back in under another name: its files, by their new paths.
    std::filesystem::rename(outside / "New", root / "Back");
    require(reported(watcher, file(root / "Back" / "CD1" / "03.flac")),
            "a folder moved in has its files reported");

    std::filesystem::remove(root / "Existing" / "01.flac");
    require(reported(watcher, file(root / "Existing" / "01.flac")), "a file deleted is reported");

    const auto present = watch::present_files({.local = root.native(), .engine = "/engine"});
    require(present.size() == 3U &&
                std::ranges::any_of(present,
                                    [](const auto& entry) {
                                        return entry.path == "/engine/Back/CD1/03.flac" &&
                                               entry.size == 5U;
                                    }),
            "the files here, named for the engine, with their sizes");
}

} // namespace

int main() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("trackknife-watch-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directories(base);
    a_folder_is_mapped_to_where_the_engine_sees_it();
    catching_up_finds_what_changed_while_nobody_watched();
    a_tree_is_watched_as_it_changes(base);
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
    std::cout << "watch: 3 scenarios\n";
    return EXIT_SUCCESS;
}
