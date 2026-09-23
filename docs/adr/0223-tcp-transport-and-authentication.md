# ADR-0223: TCP transport and authentication

Status: Accepted

## Context

ADR-0220 Phase 3 makes the engine reachable from another machine. Until now
the only transport is a unix socket, and its access control is the
filesystem's: the socket lives in `$XDG_RUNTIME_DIR`, so only its owner can
connect. That is the right answer for a local engine and no answer at all for
a remote one.

A TCP listener has no such protection, and this includes loopback. Any local
user can reach `127.0.0.1`, so "no authentication on loopback" would widen who
can control the engine compared with the unix socket, not merely keep things
convenient.

## Decision

**The unix socket is unchanged.** Filesystem permissions remain its
authentication, and a unix connection is trusted from its first line.

**TCP is opt-in.** The engine listens on TCP only when started with
`--listen HOST:PORT`. Nothing about a default installation opens a port.

**Every TCP connection authenticates, loopback included,** with a shared
token, before anything else:

```json
{"id": 1, "method": "session.authenticate", "params": {"token": "…"}}
{"id": 1, "result": {"authenticated": true}}
```

Until it has, every other request is answered with the error code
`unauthorized`, and the connection receives no events. An unauthenticated peer
learning what is playing is a leak in its own right, so the broadcast skips it.

A wrong token is answered once and the connection is closed. Retrying on the
same connection is refused rather than rate-limited: a client that has the
token gets it right first time, and closing costs a guesser a reconnect per
attempt. The comparison is constant-time.

**The token** is 32 random bytes, hex-encoded, generated on the first start
with `--listen` into `engine.token` in the state directory with mode `0600`,
and reused after that. Its path is printed at startup. A client is given the
token through its settings. Rotating it is deleting the file and restarting
the engine.

**No built-in TLS.** The expected deployments are a home network, and a
phone reaching the engine either through WireGuard or through a relay on a
VPS. In the first two the network is already trusted or already encrypted --
WireGuard encrypts below TCP, so plain TCP inside the tunnel is a proper
setup rather than a compromise -- and mandatory TLS would buy nothing but
certificates to manage. Where encryption is wanted on top, it is left to a
reverse proxy -- stunnel, an nginx or Caddy stream proxy, or an SSH tunnel --
in front of a loopback listener. That keeps TLS configuration, certificates
and their renewal out of the engine, where they would be a second thing to
get right.

The consequence is stated plainly rather than left to be discovered: **without
a proxy, the token crosses the network in the clear**, and anyone who can
observe the traffic can take it. On a home network or inside a WireGuard
tunnel that is an acceptable trade; across the internet without a tunnel, put
TLS in front. The engine does not
refuse a non-loopback bind without TLS, because it cannot tell whether a
proxy is doing that job.

## What this does not do

- **Per-client identities or permissions.** One token, full control. A
  client with the token is the owner. Distinguishing a read-only remote from a
  controlling one is a later decision, and nothing here prevents it: the
  handshake already names the credential.
- **Byte access.** Artwork, waveform peaks and audio for clients that cannot
  read the library are the other half of Phase 3 and belong in their own ADR.
  Until then a remote client can control playback and browse the catalogue but
  cannot fetch a cover.
- **Replay protection.** Without TLS the token can be captured and reused;
  with it, the proxy's session handles that. A challenge-response scheme would
  protect the token itself on a plain connection, at the cost of the protocol
  no longer being debuggable with `nc` -- which ADR-0222 treats as a
  requirement. Not done now; the handshake method is where it would go.

## Verification

- An unauthenticated TCP connection is refused every method except
  `session.authenticate`, and receives no events.
- A wrong token is refused and the connection closed; a right one admits it.
- The unix socket needs no handshake.
- The token file is created `0600`, reused across starts, and never replaced
  while it exists.
