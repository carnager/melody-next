// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "settings.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <functional>

namespace trackknife::daemon {

// ADR-0286: the engine's own melodyd.conf over the protocol, for a client
// that sets an engine up -- Trackknife's for this computer's, or one
// elsewhere. A password is never sent back, only that one is set.
//
// - config.get {} -> {path, exists, settings, secrets, engines:[{name,
//   settings, secrets}], from_flags}
// - config.set {changes:[{key, value|null, engine?}]} -> {path, restart,
//   overridden}: all of them or none, checked as melodyd reads the file,
//   written with the rest of the file kept; null takes a key out.
// - engine.restart {}: answered, then the engine stops for its service
//   manager to start it again with what the file says now.
void register_config_methods(protocol::Dispatcher& dispatcher, const Settings& settings,
                             std::function<void()> restart);

} // namespace trackknife::daemon
