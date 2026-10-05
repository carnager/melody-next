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
- Pings are answered by the engine. The app pings (below), which also keeps
  a proxy's idle timeout from ending a quiet connection.

## Amendment: the app pings (2026-10-05)

Out on mobile data the phone stopped playing after a few minutes, and
played on once unlocked. Through WireGuard it never had: WireGuard roams,
so a connection inside it survives the phone's address changing; a bare
connection over mobile data dies without a word when it changes. The
phone's speaker waited for its next track on a connection already gone,
the track ended, and with nothing playing Android let the app reach the
network only once it was in front again.

- **The app pings every 10 s** over a WebSocket, and closes a connection
  it has heard nothing on -- pongs included -- for 25 s. Reading ends, and
  the connection is made again; mid-track, the speaker's wait for the
  engine keeps the app running until it is back, and the engine plays on.
- A ping never waits behind a write stuck on a dead connection, so the
  check goes on running.
- The speaker closes its connection when a report cannot be sent, rather
  than reading on.
