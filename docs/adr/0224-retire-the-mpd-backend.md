# ADR-0224: Retire the MPD backend

Status: Accepted. Supersedes the MPD-client parts of ADR-0058, ADR-0071,
ADR-0129, ADR-0188, ADR-0190, ADR-0191 and ADR-0198.

## Decision

Trackknife no longer speaks MPD. The MPD client (`src/mpd`), its Qt models
(`src/quick`), the server library and connection dialog (`src/ui`), the paged
track model they used (`src/qtmodels`), the query translation into Melody's
filter dialect (`tkq_melody`), the `trackknife-mpd-probe` tool, and every
MPD branch in the workspace are removed.

What replaces them is an engine (ADR-0220): the same catalogue and playback,
owned by `tkengine`, reached over a unix socket or TCP (ADR-0223). A remote
library that used to be an MPD server is now an engine on that machine.

## Why now

The unified engine existed to remove the second authority. Until TCP landed,
MPD was the only way to control music on another machine, so it had to stay.
With `tkengine --listen` there is one engine to work with, and keeping MPD
meant every feature carrying two implementations — every transport control,
tab, search, rating and mode branched on "which authority is on screen".
Those branches were also where the bugs lived: the visible tab deciding
playback ownership broke jump-to-playing and let the local refresh wipe the
engine's anchors.

## What goes with it

- **The MPD Queue tab and MPD playlist tabs.** A workspace has list tabs only.
  Documents of the old `mpd` kind are skipped on restore rather than opened,
  because their items are server URIs, not files.
- **Server-side search, the server library tree, and "load MPD selection as
  local files".** The library panel and search dialog search the catalogue the
  catalogue source chose — this process's, or an engine's.
- **The Melody Last.fm authority.** Scrobbling is the client's own (and moves to
  the engine later, per the unified-engine plan).
- **The Connections settings page, the MPD music folder, and the "Start in"
  choice.** With one authority there is nothing to choose between.
- **MPD queue priorities** in the shared track delegate.

## What stays

- **The Melody agent library** (`src/audio/melody_agent`). It does not depend on
  the MPD client, and the agent stack — remote outputs a server drives — is
  kept by the unified-engine plan. Its workspace wiring went, because it was
  configured from MPD connection profiles; how an engine drives remote outputs
  is a later decision.
- **The persistence schema.** Connection profiles, MPD list documents and view
  presets remain in the workspace database untouched. Nothing reads them, and
  dropping them would make a downgrade lose data for no gain.
- **Phase 5, the MPD bridge** — the engine speaking MPD to MPD clients — is the
  opposite direction and unaffected.

## Consequences

`docs/architecture.md` and `docs/mpd-client.md` describe the retired backend
and are marked as such; the architecture document needs rewriting around the
engine rather than patching.
