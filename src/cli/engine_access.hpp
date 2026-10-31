// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// What melody-cli and melody-pick share: which engine they talk to, and how
// what was picked is put in its queue (ADR-0346). One implementation, so the
// two find the same engine from the same options and queue alike.

#include "trackknife/core/result.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/protocol/client.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::cli {

using Json = nlohmann::json;

// Which engine: --server, --engine, --local or none of them, with the
// password a TCP engine wants.
struct EngineChoice final {
    // HOST:PORT or a socket path; $MELODY_SERVER when not given.
    std::string server;
    std::string password;
    // A name an engine nearby announces.
    std::string engine;
    // This computer's engine, whatever $MELODY_SERVER says.
    bool local{false};
};

// This computer's engine's socket.
[[nodiscard]] std::filesystem::path local_socket();

// The engines on the network. Their answers come within a second, so that
// long is waited for -- unless one is wanted by name, which ends the wait
// as soon as it has answered.
[[nodiscard]] std::vector<discovery::Found> look_around(const std::string& wanted = {});

// The engine `choice` names: the server given, else this computer's engine
// when it runs, else the one engine nearby (by name, if named). `program`
// names the caller in what an older engine's notice says; a notice, when
// there is one, is left in `notice`.
[[nodiscard]] core::Result<std::unique_ptr<protocol::Client>>
connect_engine(const EngineChoice& choice, std::string_view program, std::string& notice);

// Library rows -- catalogue.query's tracks -- as queue entries, each with an
// identity of its own.
[[nodiscard]] std::vector<Json> queue_entries(const std::vector<Json>& tracks);

// The tracks of one album, by its key, or the one track with that key (its
// path) -- exactly what a picker showed.
[[nodiscard]] core::Result<std::vector<Json>> keyed_tracks(protocol::Client& client, bool album,
                                                           const std::string& key);

enum class Placement {
    // Instead of what is queued, and played.
    play,
    // After what is queued.
    add,
    // Up Next, first: what plays after this one.
    insert,
    // Up Next, last.
    upnext,
};

// `entries` put where `placement` says. The answer is the last call's:
// playback.play's for play.
[[nodiscard]] core::Result<Json> place(protocol::Client& client, Placement placement,
                                       const std::vector<Json>& entries);

} // namespace trackknife::cli
