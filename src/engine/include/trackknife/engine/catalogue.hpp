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
// What a caller may ask of a catalogue, whichever side of a socket it is on.
//
// ADR-0220: a connection profile chooses the implementation. The workspace
// programs against this and never learns which it got -- the library panel's
// queued tasks take a Catalogue&, and that is the whole seam.
//
// Every call is synchronous. Callers are already on worker threads, which is
// what lets a remote implementation simply block rather than forcing the
// workspace to become asynchronous.
//
// `scan` is here despite being a job over a socket, and that is a correction
// of an earlier decision. A job is submit/events/cancel, and a call is not --
// but the caller this exists for is already on a worker thread, already polls
// progress counters and already holds a cancellation token. That is the job
// shape assembled from local parts, so a blocking call with progress atomics
// is what it wants; the remote side adapts, rather than every caller
// reassembling the same thing.
class Catalogue {
  public:
    Catalogue() = default;
    Catalogue(const Catalogue&) = delete;
    Catalogue(Catalogue&&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;
    Catalogue& operator=(Catalogue&&) = delete;
    virtual ~Catalogue() = default;

    // The configured library roots, and adding or removing one. Mutating the
    // root set does not scan; that is a separate ask.
    [[nodiscard]] virtual core::Result<std::vector<persistence::LibraryRoot>> roots() const = 0;
    [[nodiscard]] virtual core::Result<void> add_root(const std::string& raw_path) = 0;
    [[nodiscard]] virtual core::Result<void> remove_root(const std::string& raw_path) = 0;

    // Browsing: a page of artists, albums or tracks, and the raw paths the
    // same browse would yield.
    [[nodiscard]] virtual core::Result<persistence::LibraryPage>
    query(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::string>>
    paths(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const = 0;

    // Searching: a bounded page of a compiled query.
    [[nodiscard]] virtual core::Result<persistence::LibraryPage>
    filter(const query::CompiledTkq& compiled, std::size_t offset, std::size_t limit,
           const core::CancellationToken& cancellation = {}) const = 0;

    // ADR-0179: 0-10 content-identity ratings, by hash. Reading is bulk
    // because a view asks for a screenful at once; writing is one at a time
    // because a rating is a deliberate act.
    [[nodiscard]] virtual core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation = {}) const = 0;
    [[nodiscard]] virtual core::Result<void> set_rating(const std::string& hash, bool album,
                                                        unsigned rating) = 0;

    // Raw paths matching a compiled query, in library order.
    [[nodiscard]] virtual core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq& compiled,
                 const core::CancellationToken& cancellation = {}) const = 0;

    // The stored artwork source for an album key, if the album has one. A
    // missing album and an album with no artwork are both an empty optional;
    // only an unreachable catalogue is an error.
    [[nodiscard]] virtual core::Result<std::optional<std::string>>
    artwork_source(const std::string& album_key,
                   const core::CancellationToken& cancellation = {}) const = 0;

    // Re-reads the named files now, after they were tagged or moved from
    // elsewhere: the ones in the library are indexed again, the ones gone
    // are dropped. Returns how many changed.
    [[nodiscard]] virtual core::Result<std::size_t>
    refresh(const std::vector<std::string>& raw_paths,
            const core::CancellationToken& cancellation = {}) = 0;

    // A track's cover, as encoded bytes read on the engine's machine --
    // embedded, else a folder image beside it. Only for a track in the
    // library: this reads files on the engine's machine for anyone who can
    // reach it, so it reads nothing else. Empty when there is no cover.
    [[nodiscard]] virtual core::Result<std::vector<unsigned char>>
    artwork(const std::string& raw_path, const core::CancellationToken& cancellation = {}) const = 0;

    // Cached fields and technicals for the given raw paths, in input order,
    // preserving duplicates. No filesystem access: a path missing from the
    // index is an error rather than a trigger for discovery.
    [[nodiscard]] virtual core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation = {}) const = 0;

    // Play counts and timestamps for the given sources, in input order.
    [[nodiscard]] virtual core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation = {}) const = 0;

    // Walks the configured roots and updates the index. Blocks; `progress` is
    // a set of atomic counters the caller reads while it runs.
    [[nodiscard]] virtual core::Result<persistence::LibraryScanResult>
    scan(const core::CancellationToken& cancellation,
         persistence::LibraryScanProgress& progress) = 0;
};

// The catalogue as a database this process can open.
class LocalCatalogue final : public Catalogue {
  public:
    explicit LocalCatalogue(std::filesystem::path database) : database_(std::move(database)) {}

    // Opens the catalogue once, creating and migrating it. Callers do not need
    // this -- every operation opens for itself -- but a daemon does: a
    // migration failure should surface at startup rather than in the response
    // to some client's first request, and the store should exist on disk from
    // the moment the engine says it is listening.
    [[nodiscard]] core::Result<void> prepare() const;

    [[nodiscard]] core::Result<persistence::LibraryScanResult>
    scan(const core::CancellationToken& cancellation,
         persistence::LibraryScanProgress& progress) override;

    [[nodiscard]] core::Result<std::vector<persistence::LibraryRoot>> roots() const override;
    [[nodiscard]] core::Result<void> add_root(const std::string& raw_path) override;
    [[nodiscard]] core::Result<void> remove_root(const std::string& raw_path) override;
    [[nodiscard]] core::Result<persistence::LibraryPage>
    query(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::vector<std::string>>
    paths(const persistence::LibraryQuery& request,
          const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<persistence::LibraryPage>
    filter(const query::CompiledTkq& compiled, std::size_t offset, std::size_t limit,
           const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::vector<unsigned>>
    ratings(const std::vector<std::string>& hashes,
            const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<void> set_rating(const std::string& hash, bool album,
                                                unsigned rating) override;
    [[nodiscard]] core::Result<std::vector<std::string>>
    filter_paths(const query::CompiledTkq& compiled,
                 const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::optional<std::string>>
    artwork_source(const std::string& album_key,
                   const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::vector<unsigned char>>
    artwork(const std::string& raw_path,
            const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::size_t>
    refresh(const std::vector<std::string>& raw_paths,
            const core::CancellationToken& cancellation = {}) override;
    [[nodiscard]] core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation = {}) const override;

  private:
    [[nodiscard]] core::Result<persistence::LocalLibrary> open() const;

    std::filesystem::path database_;
};

} // namespace trackknife::engine
