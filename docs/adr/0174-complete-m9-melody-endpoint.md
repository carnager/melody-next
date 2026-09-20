# ADR-0174: Complete the M9 Melody endpoint

- Status: accepted
- Date: 2026-09-16
- Owners: Trackknife project
- Extends: ADRs 0058, 0112, 0135, and 0144

## Context

Trackbench could control ordinary MPD and Melody servers, including Melody's
extended output records, but it could not itself register as an output. The
shared local playback service already owned FFmpeg decode, ReplayGain, gapless
preload, and PipeWire. M9 needed a server-authority adapter around that engine,
without routing Melody rows through the Local Queue authority or changing
stock-MPD behavior.

The current sibling `../melody` implementation defines agent protocol v2. It
registers a persistent process identity, fetches a version-consistent queue on
a secondary MPD connection, receives transport/preload commands, and reports
state and exactly one advance at each playback boundary.

## Decision

Trackbench starts a dedicated Melody agent only when the connected server
advertises its `melody_version` marker. (`agent_register` is intentionally a
special connection-takeover command and is not returned by Melody's MPD
`commands` response.) The adapter explicitly registers as protocol v2,
keeps a stable instance identity across reconnects, synchronizes the queue
between matching playlist-version reads, and handles play, preload, seek,
pause/resume, stop, volume, ReplayGain, keepalive, and the v1 property aliases
retained by Melody. It publishes the local player clock and latches chain/end
transitions so a track boundary emits one `agent_advance`.

The endpoint owns a separate `LocalAuditionService`; it therefore shares the
qualified decoder, PipeWire, ReplayGain, and gapless implementation without
borrowing the Local Queue controller. A connection profile's local music root
is preferred only for contained relative MPD paths. Otherwise the stable
Melody song ID selects `/api/v1/stream/<id>` from the configured/default HTTP
origin. Both choices retain the same visible song identity.

Disconnect stops endpoint playback, marks the output offline at the server,
and retries with a stop-aware bounded delay. Re-registration lets Melody send
its authoritative queue and playback state again. Stock MPD never starts the
adapter because it does not advertise the registration command.

Advanced listening remains a set of independent capabilities, not one hidden
M9 bundle. MPRIS/media keys and notifications are already delivered by ADRs
0135 and 0144. Album shuffle, persisted listening statistics/resume, richer
notification artwork, and a general DSP graph remain explicitly open.

## Exit-criterion reconciliation

- A protocol fixture derived from `../melody` proves v2 registration,
  secondary version-consistent queue sync, command response, playback source,
  and server-visible identity.
- Resolver tests prove contained direct paths, traversal rejection, and stable
  ID-based stream URLs; the shared player retains its existing preload and
  transition tests.
- A single transition/end latch and persistent registration instance prevent
  duplicate advancement across normal playback and reconnect generations.
- Endpoint construction is gated by Melody's advertised version marker, and the existing stock-MPD and
  Trackbench controller/UI suites pass unchanged.
- Connection profiles already persist their own raw local music root, now used
  by both explicit MPD-to-local preparation and endpoint source resolution.

M9 is complete. Live multi-machine endurance and network fault campaigns are
release-hardening work in M10, not a reason to weaken this deterministic
protocol fixture.

## Host-qualified output names (2026-09-20)

**Trackknife decision:** Register the workspace endpoint as
`Trackknife @ <hostname>`, stable across restarts. Melody keys agent devices by
name; its per-process `instance` token detects replacement but does not give
same-named devices separate identities. Two computers using the old fixed name
continually disconnected each other. The output menu identifies its own endpoint
using the actual registered name. A failed hostname lookup uses a process-stable
random fallback instead of the shared name. Distinct computer hostnames are
required; concurrent clients on the same host still represent the same output.

Validation covers host-derived default configuration, wire registration, local
playback, and HTTP streaming fixtures. Existing explicit agent names remain
supported.
