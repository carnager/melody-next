# ADR-0270: The phone reaches an engine through a TLS proxy

## Status

Accepted, 2026-10-05. Extends ADR-0223 (TCP and passwords; no built-in TLS)
and ADR-0231 (the Android client). Amended by ADR-0271 the same day: a raw
TCP proxy takes a server most people cannot build, so the `tls://` address
gave way to `wss://`; fetching streams where the phone reached the engine
stands.

## Context

Engines speak protocol v1 in the clear, by decision (ADR-0223): TLS belongs
to a proxy, or to a VPN. A phone away from home that cannot run the VPN
reaches its engine through a proxy on a server -- terminating TLS, and
forwarding over WireGuard to the engine's plain ports. But the app spoke
plain TCP only. And as a speaker, it played streams at the address the
engine gave: the engine names its own address on the connection, which
behind a proxy is its address on the VPN -- one the phone cannot reach --
with `http`, where the proxy speaks TLS.

## Decision

- **An engine's address may say TLS:** `tls://host:port`. The connection --
  the app's, and its speaker's -- is then made over TLS, the certificate
  checked against the host's name with the phone's own trust, as a browser
  does. Plain addresses are as before. Found engines are plain.
- **Streams are fetched where the phone reached the engine:** a speaker's
  stream address keeps the engine's stream port and query but takes the host
  the phone connected to, and `https` for a TLS address; offline downloads
  were already built this way and take `https` too. A proxy therefore
  forwards both ports, 6603 and 6604, at the same numbers, with TLS on both.
- **The engine does not change.** It speaks plain TCP behind the proxy.

## Consequences

- No protocol change; an engine of any level is reached so.
- Behind a plain proxy too, the speaker now fetches streams from the address
  it reached -- which a proxy needs, and which on a home network is the same
  machine as before.
- A self-signed certificate is not trusted unless the phone trusts it.
