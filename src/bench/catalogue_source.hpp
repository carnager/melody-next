// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/engine_launcher.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/client.hpp"

#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
    // ADR-0227: which of the workspace's two engines this is.
    enum class Role : std::uint8_t {
        // This computer's: always there, started if need be.
        local,
        // The configured engine elsewhere, if there is one.
        remote,
    };

    // Reads the settings once. The local role is this computer's engine,
    // started if it is not running (ADR-0226); the remote role is the one
    // configured in Settings, if any (ADR-0227). There is no fallback: an
    // engine that cannot be reached leaves its library unavailable, and says
    // why, rather than showing some other database as if it were that one.
    explicit CatalogueSource(std::filesystem::path database, Role role = Role::local);
    CatalogueSource(const CatalogueSource&) = delete;
    CatalogueSource(CatalogueSource&&) = delete;
    CatalogueSource& operator=(const CatalogueSource&) = delete;
    CatalogueSource& operator=(CatalogueSource&&) = delete;
    ~CatalogueSource();

    // Safe to call from any thread, and the result is safe to use on the
    // thread that asked. Callers are expected to be on workers already,
    // because it blocks. A dropped connection is made again here -- starting
    // this computer's engine again if that is the one -- so the library comes
    // back after an engine restart without restarting the window.
    [[nodiscard]] std::unique_ptr<engine::Catalogue> open() const;

    [[nodiscard]] bool usingEngine() const;
    // Where the engine is -- a socket or a TCP address with its token
    // (ADR-0223). Empty when no engine is configured.
    [[nodiscard]] const std::optional<protocol::Endpoint>& endpoint() const noexcept {
        return endpoint_;
    }
    [[nodiscard]] QString failure() const;
    [[nodiscard]] const std::filesystem::path& database() const noexcept { return database_; }
    // Whether the engine is the one this workspace starts for itself.
    [[nodiscard]] bool startsOwnEngine() const noexcept { return local_engine_.has_value(); }
    // Starts the local engine again if it has stopped; false when there is
    // none to start or it would not come up. Blocks while it starts.
    [[nodiscard]] bool reviveLocalEngine() const;
    // Stops this computer's engine and starts it with the settings as they
    // are now. False when there is none, or it did not come back.
    [[nodiscard]] bool restartLocalEngine() const;
    // This computer's engine, when this source started it: stopped, and
    // whether it runs an outdated program.
    [[nodiscard]] bool stopLocalEngine() const;
    [[nodiscard]] bool localEngineOutdated() const;

    [[nodiscard]] Role role() const noexcept { return role_; }
    // Whether this names an engine at all: a remote role with nothing
    // configured does not.
    [[nodiscard]] bool configured() const noexcept { return endpoint_.has_value(); }
    // What to call it in a source switch: "This computer", or the remote's
    // host.
    [[nodiscard]] QString name() const;

    // One line naming the source, for a panel to show. Three states, and the
    // difference between the last two is what was previously invisible.
    [[nodiscard]] QString describe() const;
    // Whether the library can be reached now; describe() says why not.
    [[nodiscard]] bool reachable() const;

  private:
    std::filesystem::path database_;
    Role role_{Role::local};
    std::optional<protocol::Endpoint> endpoint_;
    std::optional<LocalEngine> local_engine_;
    [[nodiscard]] std::shared_ptr<protocol::Client> connectLocked() const;

    // Replaced on reconnect, from whichever worker notices first.
    mutable std::mutex mutex_;
    mutable std::shared_ptr<protocol::Client> client_;
    mutable QString failure_;
    // The name the engine gave for itself once connected (engine.info).
    mutable QString announced_;
    // Whether the engine answered and said no, as opposed to not answering.
    // Decided from the error code: a client must never parse the message.
    mutable bool refused_{false};
};

} // namespace trackknife::bench
