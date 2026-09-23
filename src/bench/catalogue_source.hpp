// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/client.hpp"

#include <QString>

#include <filesystem>
#include <memory>
#include <optional>

namespace trackknife::bench {

// Where the workspace's catalogues come from.
//
// ADR-0220: a connection profile chooses between opening the database here and
// asking an engine, and this is the one place that choice is made. It exists
// because making it per-caller went wrong three times -- the panel's scan, its
// artwork loader, and the search dialog each reached for a database of their
// own, so routing one left the others silently local and the result was a
// library that could be browsed but not searched.
//
// Hand this around rather than a database path. A caller that has a path can
// still open the wrong thing; a caller that has this cannot.
class CatalogueSource final {
  public:
    // Reads the engine socket setting once. An engine that cannot be reached
    // is recorded and everything falls back to the local database: an
    // unreachable engine should cost the engine, not the library.
    explicit CatalogueSource(std::filesystem::path database);
    CatalogueSource(const CatalogueSource&) = delete;
    CatalogueSource(CatalogueSource&&) = delete;
    CatalogueSource& operator=(const CatalogueSource&) = delete;
    CatalogueSource& operator=(CatalogueSource&&) = delete;
    ~CatalogueSource();

    // Safe to call from any thread, and the result is safe to use on the
    // thread that asked. Callers are expected to be on workers already,
    // because both implementations block.
    [[nodiscard]] std::unique_ptr<engine::Catalogue> open() const;

    [[nodiscard]] bool usingEngine() const noexcept { return client_ != nullptr; }
    // Where the engine is -- a socket or a TCP address with its token
    // (ADR-0223). Empty when no engine is configured.
    [[nodiscard]] const std::optional<protocol::Endpoint>& endpoint() const noexcept {
        return endpoint_;
    }
    [[nodiscard]] const QString& failure() const noexcept { return failure_; }
    [[nodiscard]] const std::filesystem::path& database() const noexcept { return database_; }

    // One line naming the source, for a panel to show. Three states, and the
    // difference between the last two is what was previously invisible.
    [[nodiscard]] QString describe() const;

  private:
    std::filesystem::path database_;
    std::optional<protocol::Endpoint> endpoint_;
    std::unique_ptr<protocol::Client> client_;
    QString failure_;
    // Whether the engine answered and said no, as opposed to not answering.
    // Decided from the error code: a client must never parse the message.
    bool refused_{false};
};

} // namespace trackknife::bench
