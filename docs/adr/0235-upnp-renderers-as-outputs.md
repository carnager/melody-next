# ADR-0235: UPnP renderers as outputs (optional)

- Status: accepted and implemented; optional at build time and runtime.
- Date: 2026-09-26
- Builds on: ADR-0228 (outputs), ADR-0230 (the stream port and tickets)

## Context

The engine plays on its own speakers and on output agents. A speaker that
can't run an agent can't be played on. Many network speakers and streamers
are UPnP/DLNA MediaRenderers: they take a URL, fetch it themselves, and can
be controlled remotely.

Home Assistant came up as a way to reach such speakers. It can hand one a
URL through its own DLNA integration, but then the speaker plays a track the
engine knows nothing about, and the engine loses the queue, gapless playback,
position and history. That goes against the rule that the engine owns
playback (ADR-0220).

## Decision

**A UPnP renderer is an output like an agent.** The engine acts as the UPnP
control point. A new `audio::Audition`, next to `output::AgentAudition`,
turns the player's calls into UPnP actions, so the player can't tell a
renderer from an agent:

| Audition                        | UPnP                                        |
|---------------------------------|---------------------------------------------|
| open / play a segment           | `AVTransport.SetAVTransportURI`, `Play`     |
| `queue_gapless_next_…`          | `AVTransport.SetNextAVTransportURI`         |
| `clear_gapless_next`            | `SetNextAVTransportURI` with an empty URI   |
| `play`, `pause`, `stop`         | `Play`, `Pause`, `Stop`                     |
| `seek_to_seconds`               | `Seek` with `REL_TIME`                      |
| `set_volume_percent`            | `RenderingControl.SetVolume`                |
| `snapshot`                      | `LastChange` events (GENA), polling `GetPositionInfo` as a fallback |

`Outputs` lists renderers as `upnp:<UDN>`, keyed by the device's UDN so
that a renderer that changes its IP address is still the same output. They
are selected, restored and persisted like agents.

When playback moves between renderers, the new renderer receives the current
track and position. Position restoration is deferred until after `Play` because
some otherwise conforming renderers reject `Seek` while their transport is
stopped. Polling retries that seek while the device enters `PLAYING`.

**Found by SSDP.** Engines find each other by mDNS (ADR-0229). Renderers
announce themselves over SSDP (`urn:schemas-upnp-org:device:MediaRenderer:1`),
so that needs a small listener in `src/discovery`. It could be switched off,
for engines that shouldn't see the whole LAN's speakers.

**URLs from the stream port, one ticket per track.** The renderer fetches
from the existing HTTP stream port with a ticket (ADR-0230). The engine gets
the ticket when it sets or queues the track, so the ticket's one-hour limit
never matters. The agents' token is never put in a renderer URL, because a
renderer isn't trusted with it. The host in the URL must be an address the
renderer can reach: the engine's address on the interface where the renderer
was found, never `localhost`.

**The renderer says what it plays.** `ConnectionManager.GetProtocolInfo`
lists its sink formats. This does the job of an agent's codec list. What it
can't decode (WavPack, APE, tracker files) and every part of a file (CUE,
subsongs) is transcoded. Opus is the wrong target here, since few renderers
play it, so the converter needs a lossless preset: FLAC, or WAV/LPCM for
renderers without FLAC. It uses the same transcode cache, keyed by format.
Each file is sent with its duration in the DIDL-Lite metadata, and with
title, artist, album and cover art for renderers that have a display.

Protocol-info MIME declarations do not express every decoder limit. Sonos
advertises FLAC without its documented 48 kHz ceiling, so higher-rate sources
are delivered as lossless FLAC capped at 48 kHz. Sources already within that
limit and renderers without that identified constraint keep the original.

**ReplayGain is unsupported.** UPnP outputs play at unity gain and report
`replay_gain: false` in their output capability. The engine's ReplayGain mode
is still accepted while one is selected -- it is the engine's setting, kept
for the next output -- and simply not applied; refusing it surfaced as an
error each time a client set ReplayGain (2026-10-03). Originals therefore remain
originals when the renderer supports their format, and conversion is limited
to unsupported formats and logical track segments.

Adjusting `SetVolume` per track is ruled out: it fights the user's volume
and steps audibly at track changes.

**The feature is optional.** `TRACKKNIFE_ENABLE_UPNP` controls whether the
discovery and renderer output implementation is compiled and whether libupnp
is required. In a build that includes it, discovery is still off until the
user enables **Settings → Engine → Discover UPnP speakers**, or starts a
headless engine with `--upnp`. `--upnp-interface` restricts discovery to one
network interface. UPnP requires the HTTP stream listener on a LAN-reachable
address.

The `macos` CMake preset enables UPnP and the CoreAudio local output while
disabling the Linux-only inotify watcher. Those capabilities have independent
`TRACKKNIFE_ENABLE_LOCAL_AUDIO` and `TRACKKNIFE_BUILD_WATCH` switches, so the
same source tree can produce a smaller build without UPnP or platform-specific
helpers.

## Amendment, 2026-10-02: the engine never waits on a renderer

Merged into the main line with these changes:

- **Commands are queued.** The player calls an output under its own lock, so
  a renderer's output records what a command means for its state and
  returns. Its worker then does the transcode and the SOAP calls, which
  libupnp lets take 30 seconds against a hung speaker. A failure shows in
  the output's state, as a network fault always did. A new track drops the
  queued commands about the old one, but not the renderer's details or its
  volume.
- **A poll that started before a command doesn't overwrite it.** Every
  command that changes what plays bumps a generation, and a poll that sees
  a newer one drops what the renderer told it.
- **A track restored while paused isn't sent.** At engine start or on
  switching outputs, sending it would stop whatever the speaker was playing
  for someone else. It goes to the speaker, where it was left, when it's
  played.
- **Tickets outlive the track.** A renderer's URL is good for six hours plus
  the track's length. Within ten minutes of expiry, the output prepares the
  track again where it is, on a poll, a resume or a seek, because a renderer
  fetches again for each.
- **A local-only engine serves renderers on one address.** With `--upnp` and
  `--local-only`, `--http 0.0.0.0:PORT` is narrowed to the discovery
  interface's address, so the stream port isn't open on every network.
- **Optional at build time means `AUTO`.** `TRACKKNIFE_ENABLE_UPNP` builds
  UPnP where libupnp is installed and leaves it out where it isn't. `ON`
  requires it.

## Out of scope

- **Home Assistant.** An HA integration is a separate project: a Python
  `media_player` entity that speaks protocol v1 to the engine. It chooses
  outputs, including renderers, through `outputs.select`, and never
  addresses a speaker itself.
- **Roon.** Roon doesn't send audio to UPnP renderers, and nothing can be
  pushed into a Roon zone from outside. A speaker shared with Roon is shared
  by whatever other protocol it supports (AirPlay, Chromecast, Roon Ready),
  and the last one to start playing takes it over.
- **Acting as a renderer** (other control points playing on melody's
  speakers). That's the reverse direction, and a different decision.

## Consequences

- Speakers that can't run an agent become outputs, with the engine keeping
  the queue, gapless playback where the renderer supports
  `SetNextAVTransportURI`, position and history.
- Renderers vary in quality. Some ignore `SetNextAVTransportURI`, send no
  events, or report wrong positions, so the audition needs a fallback for
  each (gapped track changes, polling), and probably a list of known quirks.
- The stream port must be reachable from the LAN, not just from agents.
- The converter gains a lossless target, and the transcode cache holds more
  than Opus.
