# ADR-0239: A desktop agent asks for Opus when its engine is far away

## Status

Accepted, 2026-09-29. Extends ADR-0228 (output agents) and ADR-0234
(engines elsewhere).

## Context

An output agent without its own copy of the music streams it from the
engine (ADR-0228). The engine can send the original file or Opus at a bit
rate, and an agent says which it wants when it registers (`stream:
{format: "opus", bitrate: N}`). The Android app does this: the original on
Wi-Fi and Opus on mobile data, each rate chosen in its Settings.

The desktop agents never asked. That covers `melody-agent`, `melodyd
--play-for` and `melodyd --agent`, so they always took the original. A
laptop playing for the engine at home over WireGuard pulled full FLAC
through the tunnel.

A desktop has no "metered" flag worth trusting, and the same laptop sits
next to the engine one day and far from it the next. Asking the user to
list hosts or networks would be a setting that is wrong half the time.

## Decision

**The agent decides from the connection it just made.** After connecting,
and before registering, it looks at the local end of the socket: which
interface carries it, and whether the engine is on that interface's own
network.

- A unix socket, loopback, or an engine on the same subnet as a plain
  interface (Ethernet, Wi-Fi) is **nearby**.
- A point-to-point interface (WireGuard, tun, PPP, Tailscale), or an
  engine reached through a router, is **away**.
- When it cannot tell, it treats the engine as away: poorer sound is
  better than a tunnel filled with FLAC.

It decides again on every reconnect. Moving between home and away changes
the source address, which breaks the TCP connection, so the next
registration asks again. Nothing has to watch the network.

**Two rates, like the phone's.** Nearby defaults to the original files;
away defaults to Opus 128 kbps. Each is `0` for the original, or an Opus
rate from 16 to 512 kbps:

- `melody-agent --bitrate-nearby N --bitrate-away N`
- `melodyd --play-for-bitrate-nearby N --play-for-bitrate-away N`,
  per `--play-for` like its other options, before any for all of them
- `melodyd --agent-bitrate-nearby N --agent-bitrate-away N`

**Settings** has the two rates under "Let other engines play on this
computer's speakers". Each engine elsewhere has "Streamed here":
*Automatic* (the two rates), or one fixed choice for that engine whatever
the route. Changing any of them restarts this computer's engine, as its
other sharing settings do.

The wish is sent only by an agent that streams. One with the files opens
them, so no rate applies to it. The agent's log says which route it saw
and what it asked for, so a wrong guess is visible.

## Consequences

- The engine and the protocol are unchanged.
- A metered connection on a desktop (tethering) is not detected. That is
  NetworkManager's to say, over D-Bus; it can later push "away" when the
  route alone says "nearby".
- A tap-mode VPN looks like Ethernet and counts as nearby when the engine
  is on its subnet. A fixed per-engine choice covers it.
