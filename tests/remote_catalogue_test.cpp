// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the same interface, backed locally and remotely, answering the
// same. This is the test that makes a connection profile a real choice rather
// than a hope -- the workspace programs against engine::Catalogue, so anything
// the two implementations disagree about is a bug the workspace would see.

#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/remote_catalogue.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = trackknife::engine;
namespace protocol = trackknife::protocol;
namespace core = trackknife::core;
namespace query = trackknife::query;

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
}

void what_the_remote_does_not_expose_says_so(engine::RemoteCatalogue& remote) {
    // Not every method is on the wire yet. The gap is reported as unsupported
    // and names the method, rather than arriving as an empty success that
    // would look like a library with nothing in it.
    const auto tracks = remote.cached_tracks({"/music/a.flac"});
    require(!tracks, "an unexposed method must fail");
    require(tracks.error().code == core::ErrorCode::unsupported, "as unsupported");
    require(!tracks.error().context.empty(), "naming the method");
    require(tracks.error().context[0].value == "catalogue.cached_tracks", "which method it was");
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
    (*server)->start();

    auto client = protocol::Client::connect(socket);
    require(client.has_value(), "the client must connect");
    engine::RemoteCatalogue remote{**client};

    a_catalogue_behaves_the_same_either_side(local, "local");
    a_catalogue_behaves_the_same_either_side(remote, "remote");
    what_the_remote_does_not_expose_says_so(remote);

    (*client)->close();
    (*server)->stop();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "remote catalogue: both sides agree\n";
    return EXIT_SUCCESS;
}
