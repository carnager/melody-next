// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::engine {

// The core's front door for catalogue reads.
//
// ADR-0220: the UI asks the core to do something; the core owns the data,
// looks it up, and returns what was asked for. Callers pass what they want to
// know, never where it is stored -- no database path leaves this class, and
// nothing in its interface mentions Qt.
//
// Every call is synchronous and opens its own connection, which is exactly
// what the callers did inline before. Threading stays with the caller for now:
// in-process that is a worker pool, and in Phase 2 it becomes the protocol's
// concern. Keeping the connection strategy behind this boundary is the point --
// it can become a pool, or a socket, without any caller changing.
class Catalogue final {
  public:
    explicit Catalogue(std::filesystem::path database) : database_(std::move(database)) {}

    // Opens the catalogue once, creating and migrating it. Callers do not
    // need this -- every operation opens for itself -- but a daemon does: a
    // migration failure should surface at startup rather than in the response
    // to some client's first request, and the store should exist on disk from
    // the moment the engine says it is listening.
    [[nodiscard]] core::Result<void> prepare() const;

    // The configured library roots, and adding or removing one. Mutating the
    // root set does not scan; that is a separate ask.
    [[nodiscard]] core::Result<std::vector<persistence::LibraryRoot>> roots() const;
    [[nodiscard]] core::Result<void> add_root(const std::string& raw_path);
    [[nodiscard]] core::Result<void> remove_root(const std::string& raw_path);

    // Browsing: a page of artists, albums or tracks, and the raw paths the
    // same browse would yield.
    [[nodiscard]] core::Result<persistence::LibraryPage>
    query(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const;
    [[nodiscard]] core::Result<std::vector<std::string>>
    paths(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const;

    // Searching: a bounded page of a compiled query.
    [[nodiscard]] core::Result<persistence::LibraryPage>
    filter(const query::CompiledTkq& compiled, std::size_t offset, std::size_t limit,
           const core::CancellationToken& cancellation = {}) const;

    // ADR-0179: 0-10 content-identity ratings, by hash. Reading is bulk
    // because a view asks for a screenful at once; writing is one at a time
    // because a rating is a deliberate act.
    [[nodiscard]] core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation = {}) const;
    [[nodiscard]] core::Result<void> set_rating(const std::string& hash, bool album,
                                                unsigned rating);

    // Raw paths matching a compiled query, in library order.
    [[nodiscard]] core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq& compiled,
                 const core::CancellationToken& cancellation = {}) const;

    // The stored artwork source for an album key, if the album has one. A
    // missing album and an album with no artwork are both an empty optional;
    // only an unreachable catalogue is an error.
    [[nodiscard]] core::Result<std::optional<std::string>>
    artwork_source(const std::string& album_key,
                   const core::CancellationToken& cancellation = {}) const;

    // Cached fields and technicals for the given raw paths, in input order,
    // preserving duplicates. No filesystem access: a path missing from the
    // index is an error rather than a trigger for discovery.
    [[nodiscard]] core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation = {}) const;

    // Play counts and timestamps for the given sources, in input order.
    [[nodiscard]] core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation = {}) const;

    // Walks the configured roots and updates the index. Long, mutating, and
    // cancellable; `progress` is a set of atomic counters the caller reads
    // while this runs.
    //
    // ADR-0220 calls operations like this **jobs** -- submit, observe
    // progress, cancel, collect a result -- and that shape already exists at
    // the call site, assembled from Qt parts: a worker pool submits, a timer
    // polls the counters, a token cancels, a watcher delivers the result.
    // Passing it through the door keeps that shape and takes the database path
    // out of the UI, which is what Phase 1 is for. What Phase 2 changes is
    // where the thread lives and whether progress is pushed rather than
    // polled -- not the shape here. Building engine-owned threading now would
    // duplicate the caller's pool and design the job machinery without the
    // socket that is its actual requirement.
    [[nodiscard]] core::Result<persistence::LibraryScanResult>
    scan(const core::CancellationToken& cancellation, persistence::LibraryScanProgress& progress);

  private:
    [[nodiscard]] core::Result<persistence::LocalLibrary> open() const;

    std::filesystem::path database_;
};

} // namespace trackknife::engine
