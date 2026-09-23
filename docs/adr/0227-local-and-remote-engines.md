# ADR-0227: This computer's engine and a remote one

- Status: accepted
- Date: 2026-09-23
- Amends: ADR-0226 (withdraws "one engine per workspace")
- Restores: the unified-engine plan's separate local and remote tabs (Phase 3)

## Context

ADR-0226 made the engine setting choose one engine, local or remote. Naming
a remote engine therefore replaced this computer's: local files no longer
played, because the only engine was one that did not have them. The plan had
always kept local and remote as separate tabs for exactly this reason.

## Decision

**Two connections.** This computer's engine is always there (ADR-0226 starts
it). A remote engine, when one is configured in Settings → Library, is
connected beside it. There is at most one remote.

**Tabs belong to a connection.** A list document carries a `remote` flag
(schema 43). A local tab's files are on this computer and play on its engine;
a remote tab's files are on the remote machine and play there. Entries do not
move between the two, because a path means a file only on its own machine.
Remote tabs show a server icon. Connecting to a remote opens its default tab,
named after it, and a queue the remote was already playing is adopted into
it.

**One engine plays at a time.** Playing a tab makes the transport follow that
tab's engine and stops the other one first. Volume, output device, buffer and
modes shown are those of the engine the transport follows. A remote that was
already playing when the window connected is followed, unless this computer
is playing.

**One library switch.** The folders panel's source switch gains the remote
library beside Folders and Library. Actions from it fill remote tabs (the
remote tab on screen, else the remote's own), and search results become
remote tabs. The rows come from the remote engine's index: what the files are
is told by the engine that has them, never read here.

**Up Next holds one engine's asks.** Its tracks are files on one machine, so
it takes them from one connection until it is empty again; mixing is refused
with a message. Asks are stated to their engine only while it is the one
playing.

**Nothing here reads a remote tab's files.** Remote rows are not probed and
their covers are not loaded from local disk. Covers for remote tabs, and a
copy of remote tracks to this computer, need the byte access Phase 3 plans:
bytes streamed over the protocol, with no file mounts.

## Lists

Tabs, scratch lists and search results are workspace state and stay in
Trackknife, like window layout. Saved playlists belong on the engine that
owns their files, like MPD's stored playlists: a playlist of NAS files only
means something there, every client should see it, and the engine can keep it
correct when it renames or moves a file. That move comes with the file-work
step below.

## File work on remote files

Tagging and ReplayGain move into the engine next, one code path for local and
remote: the client sends the plan it already builds, and the engine checks it
against its files and writes with its own journal. Over a network share every
tag write rewrites a file across the network; next to the disk it is a local
write. Renames and moves follow once a picker can browse the remote
filesystem, which choosing a remote library folder needs too.

The converter stays on this computer. A desktop encodes several times faster
than a NAS-class machine, the output is usually wanted here, and on a home
network moving a FLAC album costs seconds. It will read remote sources over
the protocol, from the same byte access that a plain copy uses. Converting on
the remote, into a folder there, waits until someone needs a NAS-side mirror.

Until the file work moves, the file tools on a remote tab act on the same path
on this computer: that works where the path is mounted, as a network share
did, and otherwise finds no file and changes nothing.
