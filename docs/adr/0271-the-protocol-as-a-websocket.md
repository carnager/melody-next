# ADR-0271: The protocol as a WebSocket, for clients behind a web proxy

## Status

Accepted, 2026-10-05. Extends ADR-0222 (protocol v1) and ADR-0223 (TCP and
passwords); amends ADR-0270.

## Context

ADR-0270 let the phone reach an engine through a proxy speaking TLS. But the
protocol is not HTTP, so the proxy had to carry raw TCP: nginx's `stream`,
or Caddy only with a module compiled in -- and a stock Caddy, the web server
people already run, cannot. Melody's old daemon met the same need by speaking
over a WebSocket.

## Decision

- **The engine serves the protocol as a WebSocket** at `/protocol` on its
  stream port (`--http`, 6604): each text message is one protocol message,
  both ways, exactly the lines of its TCP port. Behind the upgrade, the
  connection is the TCP listener's -- the password asked first, a wrong one
  answered and closed. Without a TCP listener with a password there is no
  WebSocket: a TCP peer needs a password, and so does this one.
- **One site carries everything:** the protocol and the streams are on the
  same port, so a plain HTTPS reverse proxy to it serves a phone entirely --
  `reverse_proxy 10.10.10.200:6604`.
- **The app takes `wss://host[:port][/path]`** (and `ws://`): it connects
  with TLS checked against the host's name, makes the WebSocket handshake
  itself -- a small client, no new library -- and fetches streams from the
  same site. `tls://` (ADR-0270) is gone.

## Consequences

- Protocol level 6. An older engine has no `/protocol`; the app says the
  server did not take the WebSocket.
- Pings are answered by the engine; nothing pings on its own, so a proxy's
  idle timeout may end a quiet connection, which the app then makes again.
