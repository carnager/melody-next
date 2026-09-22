// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/client.hpp"

namespace trackknife::engine {

// The catalogue of an engine reached over a socket.
//
// ADR-0220: a connection profile chooses between this and LocalCatalogue, and
// the workspace cannot tell which it has. Every call blocks on a round trip,
// which is only affordable because callers are already on worker threads --
// the library panel's pool, the search dialog's runner. Calling one of these
// from a UI thread would be a mistake, and the same mistake it would be with
// the local one, which opens a database.
//
// The client must outlive this.
class RemoteCatalogue final : public Catalogue {
  public:
    explicit RemoteCatalogue(protocol::Client& client) : client_(&client) {}

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
    [[nodiscard]] core::Result<std::vector<persistence::LibraryTrackSnapshot>>
    cached_tracks(const std::vector<std::string>& raw_paths,
                  const core::CancellationToken& cancellation = {}) const override;
    [[nodiscard]] core::Result<std::vector<std::array<std::int64_t, 6>>>
    history_facts(const std::vector<persistence::LibraryHistorySource>& sources,
                  const core::CancellationToken& cancellation = {}) const override;

  private:
    protocol::Client* client_;
};

} // namespace trackknife::engine
