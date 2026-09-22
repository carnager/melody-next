// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the same interface, backed locally and remotely, answering the
// same. This is the test that makes a connection profile a real choice rather
// than a hope -- the workspace programs against engine::Catalogue, so anything
// the two implementations disagree about is a bug the workspace would see.

#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/remote_catalogue.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
namespace core = trackknife::core;
namespace query = trackknife::query;
namespace persistence = trackknife::persistence;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// Every assertion runs against whichever implementation it is handed, so a
// divergence shows up as the same case failing on one side only.
void a_catalogue_behaves_the_same_either_side(engine::Catalogue& catalogue,
                                              const std::string& label) {
    const auto roots = catalogue.roots();
    require(roots.has_value(), label + ": roots must succeed");
    require(roots->empty(), label + ": a fresh catalogue has no roots");

    auto compiled = query::compile_tkq("artist HAS nothing-matches-this");
    require(compiled.has_value(), label + ": the query must compile");
    const auto paths = catalogue.filter_paths(*compiled);
    require(paths.has_value(), label + ": a query matching nothing still succeeds");
    require(paths->empty(), label + ": and returns no paths");

    // Ratings are 64-character content hashes; an unrated one reads back as
    // zero rather than as an absence the caller must interpret.
    const std::string rated(64U, 'a');
    const std::string unrated(64U, 'b');
    require(catalogue.set_rating(rated, false, 7).has_value(), label + ": storing a rating");
    const auto ratings = catalogue.ratings({rated, unrated});
    require(ratings.has_value(), label + ": reading ratings");
    require(ratings->size() == 2U, label + ": one per hash, in order");
    require((*ratings)[0] == 7U, label + ": the stored rating comes back");
    require((*ratings)[1] == 0U, label + ": an unrated hash is zero");

    // A missing cover is a success carrying nothing, not a not_found.
    const auto artwork = catalogue.artwork_source("no-such-album");
    require(artwork.has_value(), label + ": a missing cover is not an error");
    require(!artwork->has_value(), label + ": and carries no source");

    // A malformed rating identity is refused by the engine either way, so a
    // client cannot store a row nothing can find.
    const auto bad = catalogue.set_rating("too-short", false, 3);
    require(!bad, label + ": a short hash is refused");
    require(bad.error().code == core::ErrorCode::invalid_argument, label + ": as a bad argument");

    // Browsing an empty library is a success with nothing in it.
    const auto artists = catalogue.query({});
    require(artists.has_value(), label + ": browsing must succeed");
    require(artists->entries.empty(), label + ": an empty library has no artists");
    require(!artists->more, label + ": and no further page");

    const auto browse_paths = catalogue.paths({});
    require(browse_paths.has_value(), label + ": listing paths must succeed");
    require(browse_paths->empty(), label + ": with nothing to list");
}

// Adding a folder and scanning it, which is the whole point of pointing at an
// engine: a remote scan must reach the engine's files, report progress from
// its events, and leave the library populated.
void adding_and_scanning_a_folder_works(engine::Catalogue& catalogue, const std::string& label,
                                        const std::filesystem::path& music) {
    require(catalogue.add_root(music.string()).has_value(), label + ": adding a root");
    const auto roots = catalogue.roots();
    require(roots.has_value() && roots->size() == 1U, label + ": the root is listed");
    require((*roots)[0].raw_path == music.string(), label + ": with the path given");

    persistence::LibraryScanProgress progress;
    const auto scanned = catalogue.scan({}, progress);
    require(scanned.has_value(), label + ": scanning must succeed");
    require(!scanned->cancelled, label + ": and not report a cancellation nobody asked for");
    // The counters the caller polls must be filled either way; a remote scan
    // that finishes silently is indistinguishable from one that did nothing.
    require(progress.visited.load() > 0U, label + ": the scan visited something");

    // Searching is what the library search box runs, and it is the step that
    // finds results at all -- cached facts are only useful once something has
    // been found.
    auto search = query::compile_tkq("anthrax");
    require(search.has_value(), label + ": the search must compile");
    const auto found = catalogue.filter(*search, 0U, 50U);
    require(found.has_value(), label + ": searching must succeed");
    require(!found->more, label + ": a small library has no further page");

    // Cached facts must survive the crossing, or a search result against an
    // engine comes back as paths with no metadata -- which reads as a broken
    // library rather than a missing method.
    persistence::LibraryQuery tracks_query;
    tracks_query.kind = persistence::LibraryEntryKind::track;
    const auto indexed = catalogue.paths(tracks_query);
    require(indexed.has_value(), label + ": listing indexed tracks");
    if (!indexed->empty()) {
        const auto facts = catalogue.cached_tracks(*indexed);
        require(facts.has_value(), label + ": reading cached facts");
        require(facts->size() == indexed->size(), label + ": one per path, in order");
        require((*facts)[0].raw_path == (*indexed)[0], label + ": paths survive exactly");
        // Absent history is null, not zeroes: unavailable and never-played are
        // different answers.
        require(!(*facts)[0].facts.history.has_value() || (*facts)[0].facts.history->size() == 6U,
                label + ": history is absent or complete");
    }

    require(catalogue.remove_root(music.string()).has_value(), label + ": removing a root");
    const auto after = catalogue.roots();
    require(after.has_value() && after->empty(), label + ": the root is gone");
}

void what_the_remote_does_not_expose_says_so(engine::RemoteCatalogue& remote) {
    // Not every method is on the wire yet. The gap is reported as unsupported
    // and names the method, rather than arriving as an empty success that
    // would look like a library with nothing in it.
    const auto history = remote.history_facts({});
    require(!history, "an unexposed method must fail");
    require(history.error().code == core::ErrorCode::unsupported, "as unsupported");
    require(!history.error().context.empty(), "naming the method");
    require(history.error().context[0].value == "catalogue.history_facts", "which method it was");
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-remote-catalogue-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);

    // The local side, and the engine that serves the remote side, share one
    // database: the point is that the same data answers the same either way.
    engine::LocalCatalogue local{directory / "library.sqlite3"};
    require(local.prepare().has_value(), "the catalogue must open");

    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, local);
    const auto socket = directory / "engine.sock";
    auto server = engine::Server::listen(socket, dispatcher);
    require(server.has_value(), "the engine must bind");

    // A remote scan is a job, so the engine must actually offer jobs. Without
    // these the submission answers `unsupported`, which is what a client
    // talking to an engine that does not scan would correctly see.
    engine::JobRegistry jobs{(*server)->sink()};
    engine::JobCatalog job_catalogue;
    engine::register_catalogue_jobs(job_catalogue, local);
    engine::register_job_methods(dispatcher, jobs, job_catalogue);
    (*server)->start();

    auto client = protocol::Client::connect(socket);
    require(client.has_value(), "the client must connect");
    engine::RemoteCatalogue remote{**client};

    a_catalogue_behaves_the_same_either_side(local, "local");
    a_catalogue_behaves_the_same_either_side(remote, "remote");

    // A folder with one real file, so a scan has something to find.
    const auto music = directory / "music";
    std::filesystem::create_directory(music);
    {
        std::ofstream track{music / "track.flac", std::ios::binary};
        track << "not really a flac, but a file the walk must visit";
    }
    adding_and_scanning_a_folder_works(local, "local", music);
    adding_and_scanning_a_folder_works(remote, "remote", music);

    what_the_remote_does_not_expose_says_so(remote);

    (*client)->close();
    (*server)->stop();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "remote catalogue: both sides agree\n";
    return EXIT_SUCCESS;
}
