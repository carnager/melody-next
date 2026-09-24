# ADR-0231: The Android client

- Status: accepted
- Date: 2026-09-24
- Implements: the unified-engine plan's Phase 7 for Android

## Context

The old Android app talked to the Go Melody daemon: MPD commands over a
WebSocket, an HTTP API for covers and transcoded streams, and the old
agent protocol. All three are gone (ADR-0224, ADR-0228). Its screens and its
Media3 service were worth keeping; its data layer was not.

## Decision

**Rebuilt in this repository, on protocol v1.** The app lives in `android/`
(Kotlin, Compose, Media3) with its own application id, so it installs next to
the old one while both exist. It speaks protocol v1 over plain TCP, the same
JSON lines as every other client, on two connections: one for control, one
for covers, so a slow cover never holds up a skip.

**Away from home is a VPN.** The engine gets no WebSocket and no TLS for the
phone's sake. Over WireGuard or Tailscale the phone reaches the engine
as it does at home.

**Covers are scaled by the engine.** The phone asks for a cover at the size
it shows it, by album key, and keeps it on disk. A 300 px cover is about
30 KB where the originals were megabytes.

**The phone is an agent.** It registers like `melody-agent --stream`: no
files, the engine sends stream URLs, ExoPlayer plays them. A gapless next
track is a second playlist item, and reports go out four times a second
while playing. The engine keeps deciding everything, so the phone behaves
like any other output. ReplayGain is applied as player volume, held under
the track's peak.

**The lock screen mirrors the engine.** The media session's player is a stand-in
that shows the engine's state and sends every button press to the engine.
It plays nothing itself.

**Offline albums are the phone's own.** Kept albums are downloaded with
tickets (ADR-0230), each into a folder with the library's description of it and
the cover, and play on a separate player with its own media session. When
the engine plays a track the phone has kept, the agent plays the copy
instead of the stream.

## Consequences

- Gains above 0 dB are capped: Android's player volume doesn't go past full
  scale.
- The phone stays registered while the app is open or the engine is playing.
  Idle in the background, Android may drop the connection, and it shows as
  offline until the app is opened again.
- Browsing the library needs the engine. Only kept albums are there without
  it.
- The queue can't be reordered by dragging yet.
