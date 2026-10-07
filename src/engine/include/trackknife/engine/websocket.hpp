// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <string>
#include <string_view>

namespace trackknife::engine {

// ADR-0271: protocol v1 over a WebSocket, for a client that reaches the
// engine through an HTTP proxy -- one JSON message in each text message,
// both ways, as each is one line on a socket.

// The Sec-WebSocket-Accept answering a client's Sec-WebSocket-Key.
[[nodiscard]] std::string websocket_accept(std::string_view key);

// Carries a WebSocket on `client` -- its handshake answered -- to and from a
// protocol connection on `inner`: each text message the client sends becomes
// a line there, each line from there a text message. Pings are answered,
// a close is returned. Ends when either side does, or `running` turns false;
// closes `inner`, not `client`. `pending` is what the client sent after its
// handshake, already read.
void bridge_websocket(int client, int inner, std::string pending, const std::atomic<bool>& running);

} // namespace trackknife::engine
