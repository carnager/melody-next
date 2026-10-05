# ADR-0272: The phone at home reaches the engine directly

## Status

Accepted, 2026-10-05. Extends ADR-0271 (the protocol as a WebSocket) and
ADR-0234 (several engines).

## Context

A phone set up with `wss://music.example.org` reaches the engine through the
web proxy everywhere -- at home too, where the engine is on the same
network: every request and every stream out to the proxy and back in, FLAC
through a VPS. A second engine entry for home would be two engines in the
app, with two of everything.

The engine already gives a stable id in `engine.info` and announces the same
id over multicast DNS (`_melody._tcp`, TXT `id`), so a client reaching it two
ways knows it is one engine.

## Decision

- **One engine, reached the nearer way.** The app looks for engines on the
  network the phone is on all along, not only while setting up. When one is
  announced with the id of an engine it knows by another address, it reaches
  that engine at the announced address, with the same password; otherwise at
  the address saved.
- **The id is remembered** with the saved address, learnt from `engine.info`
  on connecting, so the first connection at home is already the direct one.
- **Coming home switches.** A connection made the long way is made again the
  nearer way once the engine is found here. The phone's speaker waits until
  nothing plays on it, so no track is cut off.
- **Leaving home falls back.** When Wi-Fi or Ethernet comes or goes, a
  connection made the nearer way is made again; one whose polls go
  unanswered is too -- a network left behind breaks a connection without
  closing it. An announcement outlives the phone's leaving, so the nearer way
  is given two seconds, then the saved address is tried; an engine found but
  not answering is passed over until what is found, or the network, changes.
- Streams follow the connection: fetched from where the engine was reached
  (ADR-0270), so at home the original files come over the home network.

## Consequences

- No setting: the saved address is the engine, home is found.
- A phone at home without multicast DNS (a guest network that filters it)
  keeps the long way, as before.
- Listed other engines (ADR-0234) are reached the nearer way too, once their
  id is learnt in a session.
