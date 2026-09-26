# ADR-0234: Engines are connections, not a switch

Status: Accepted (2026-09-26)

Carries out the "local versus remote is a connection, not a branch" rule of
AGENTS.md and ADR-0220 for Trackknife's side.

## Context

Trackknife reaches at most two engines: this computer's, and one remote
configured in Settings. Which one a thing belongs to is a `bool remote`
threaded through about 230 places in 20 files -- lists, library panels,
drops, searches, list sync, Up Next -- and persisted as `list_documents.remote`.
Every feature that touches an engine is written once with that flag in hand,
and a second remote (the desktop library and the NAS side by side, plus a
laptop) cannot be expressed at all.

An engine also has no lasting identity: `engine.info` and the mDNS
announcement carry an id minted afresh each time melodyd starts, so nothing a
client stores can say "this list is gemenon's" in a way that survives a
restart -- only "this list is the remote's", whichever that is today.

## Decision

**An engine has a stable id.** Minted once and kept in its state directory
(`engine-id`), it is what `engine.info` and the mDNS announcement carry. An
engine's name can change; its id does not. Copying a state directory copies
the id, which is right: it is the same engine's library and lists.

**Trackknife holds a set of engine connections.** Each is one object -- its
catalogue link, playback connection, library panel, list sync state and
remote-mount mapping -- and code that needs an engine is handed that object,
not a flag. This computer's engine is the first of them and otherwise not
special.

**Lists name their engine by id.** `list_documents` records the owning
engine's id instead of `remote`. A list of the remote configured today is
migrated to that remote's id when it is first reached; until then it keeps a
placeholder naming the configured address, so nothing is lost while the
remote is away.

**Several remotes.** Settings keeps a list of engines, each with its
address, password and mount mapping. The library sidebar, the tab groups and
the Lists panel show one group per engine, as they already do for one.

## Stages

Each stage leaves the application working and is committed on its own.

1. Stable engine id (this ADR's first commit). Done.
2. The connection object: one class for an engine connection, used for this
   computer's engine and the single remote; `bool remote` replaced by it
   throughout. No change in behaviour. Done.
3. Lists persist their engine's id; migration as above. Done (schema 47).
4. Several remotes: Settings, sidebar, grouping. Done; see below.
5. The last synchronous call on the UI thread (`queueEntries()`, review item
   B2) goes with the old remote plumbing.

## Several remotes, as built (stage 4)

- **Settings list them.** Each: an address, a password (empty: this
  computer's engine's), its music folder as it sees it, and -- optional --
  where that folder is also reachable on this computer, when the user has
  mounted it; Trackknife mounts nothing. "On the network" adds a found
  engine, or chooses it when listed. The one remote of an older release
  becomes the first entry, and the old single-remote keys keep naming the
  first. Saved, it applies at once: an engine added is connected, one
  removed -- or given another address or password -- is let go (and
  connected again if still listed); its lists stay, as those of an engine
  not reached.
- **Keys before an id.** An engine not yet reached is known by a
  placeholder: `remote` for the first (the key an older release's remote
  lists carry), `address:<address>` for any other. When it says its id, what
  was kept under the placeholder moves to the id; an id other than the one
  kept means another engine at that address, and the lists of the one
  before stay its.
- **The sidebar has a library tab per engine**, named by it, found by its
  key; tab groups and the Lists panel follow the order of the list.
- **Per engine:** the mount and library folders used to move tracks
  between engines and for file work, the index asked to re-read changed
  files, and whether a file move has been told to it.
- **Speakers.** This computer's engine offers its speakers to every engine
  listed: melodyd takes `--play-for` again for each, with the options after
  one its own (a password, where its music is reachable here), and one
  agent connection each, sharing the speakers through its arbiter as the
  engines it finds on the network (`--agent`) do.
- **The phone's speaker** registers with its engine and every other engine
  listed in its settings, one agent connection each. It has one player, so
  each connection gets a seat on it: the newest to start playing takes it,
  the engine it was taken from is left paused where it was -- same playback,
  same place, what was armed to follow -- and playing there again takes it
  back. A connection taking up a paused track does not take the player from
  music playing on it.

## Consequences

- A client can tell two addresses of one engine apart from two engines, across
  restarts -- the CLI's `watch --all` and the phone benefit too.
- A list keeps its engine when that engine's address changes.
- The remote-only branches in list, drop and search code disappear rather
  than multiply.
