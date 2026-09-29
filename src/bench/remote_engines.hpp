// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/remote_mount.hpp"

#include <QString>

#include <vector>

namespace trackknife::bench {

// ADR-0234: an engine elsewhere that this window connects to, as Settings
// keep it. There may be several; the first is the one this computer's
// engine offers its speakers to ("play for").
struct RemoteEngineSetting final {
    // HOST:PORT, or a socket path.
    QString address;
    // Empty: this computer's engine's password, the usual case.
    QString password;
    // Where that engine's music is, as it sees it, and where the same
    // folder is reachable from this computer -- if it is, by a mount the
    // user made. Empty `reachable_at`: the same path here. Only file work
    // and moving tracks between engines need it.
    QString music_folder;
    QString reachable_at;
    // The id the engine gave when last reached; empty until then.
    QString id;
    // ADR-0239: what it streams to this computer's speakers, whatever the
    // route: kbps of Opus, 0 the original files, -1 by the route.
    int stream_kbps{-1};

    // The password to use: its own, or this computer's engine's.
    [[nodiscard]] QString effectivePassword() const;
    [[nodiscard]] RemoteMount mount() const;

    friend bool operator==(const RemoteEngineSetting&, const RemoteEngineSetting&) = default;
};

// The engines configured, in order. The first time, the one remote an
// older release kept (library/engine-socket and the rest) becomes the first
// of them.
[[nodiscard]] std::vector<RemoteEngineSetting> loadRemoteEngines();
void saveRemoteEngines(const std::vector<RemoteEngineSetting>& engines);
// Keeps the id an engine gave, found by its address.
void rememberEngineId(const QString& address, const QString& id);

} // namespace trackknife::bench
