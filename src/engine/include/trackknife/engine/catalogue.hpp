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

  private:
    [[nodiscard]] core::Result<persistence::LocalLibrary> open() const;

    std::filesystem::path database_;
};

} // namespace trackknife::engine
