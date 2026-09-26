# ADR-0232: melody-watch, telling the engine what changed on a NAS

Status: Accepted (2026-09-26)

## Context

The engine indexes what its folders hold. On a desktop those are local and a
scan is cheap. A library on a NAS is usually read by an engine on another
machine, over NFS or SMB, and there a scan walks and stats every file across
the network: minutes for a large library, repeated for every change. The
obvious fix, running the engine on the NAS, does not always work: many NAS
boxes cannot run it (no PipeWire, an old or locked-down system, too little
memory for FFmpeg and SQLite work), or their owner does not want it there.

What the NAS can always do is notice its own changes. inotify works on its
local disks; it does not work on the engine's side of an NFS mount.

The engine already has the other half. `catalogue.refresh` re-reads just the
named files: a changed file is indexed again, a new one inside a library
folder is added, and a vanished one is dropped, with no directory walked.

## Decision

**A separate small program, `melody-watch`, runs where the files are.** It
watches the music folders with inotify, gathers what changed, waits for the
changes to settle, and sends the changed paths to the engine with
`catalogue.refresh`. The engine reads only those files, over the mount.

- **Paths are mapped.** The NAS and the engine usually see the same files
  under different paths. Each folder is given as `LOCAL[=ENGINE]`: the
  watcher watches `LOCAL` and names files to the engine under `ENGINE`.
- **Only audio files are sent**, by the same extension list the index uses
  (moved from persistence into core so there is one list).
- **Folders.** A folder moved into the tree is walked on the NAS -- locally,
  where it is cheap -- and its files are sent. A folder moved or deleted out
  of it is sent as the folder: `catalogue.refresh` given a path that is gone
  now also drops every indexed track under it, provided the folder's parent
  still exists, so a stale or unmounted share on the engine's side does not
  read as everything having been deleted.
- **Catching up.** Changes made while the watcher was not running are found
  by comparison, not by a scan over the network: the watcher walks its folder
  locally and asks the engine for what it has indexed under it with a new
  method, `catalogue.inventory` -- paths with the size and modification time
  they were indexed at, a page at a time. New, changed and vanished files are
  sent. This runs when the watcher starts and after an inotify queue
  overflow, which means events were lost. If more than half of the indexed
  files seem to have vanished, the deletions are not sent and the watcher
  says so: that is a wrong mapping or a missing mount, not a deleted library.
- **The engine being away** is not a loss. Changes are kept, deduplicated,
  and sent when it can be reached again.
- **It is built in the light build.** `melody-watch` needs only core and the
  protocol client, so `-DTRACKKNIFE_CLI_ONLY=ON` builds it with `melody-cli`,
  without FFmpeg, PipeWire, SQLite or Qt.
- **It authenticates like any client** (ADR-0223): `--password-file`.

## Consequences

- Where an engine can run on the NAS, a watcher inside the engine would be
  simpler still; this program's watching code is a library so the engine can
  use it for its own folders later. Not done here.
- inotify needs a watch per directory. A large library may need
  `fs.inotify.max_user_watches` raised; the watcher says so when it runs out
  rather than silently watching part of the tree.
- A file changed on the engine's side of the mount -- tagged in Trackknife,
  say -- is already refreshed by Trackknife. The watcher sees the same change
  on the NAS and sends it again, which costs one re-read.

## Verification

- Unit tests for the path mapping, the catch-up comparison and the watcher
  on a real directory tree: a file written, a folder made with files in it, a
  folder moved in and out, a file deleted.
- Library tests: a vanished folder drops its tracks, and does not when its
  parent is gone too; `inventory` pages by path.
- An end-to-end test: `melodyd` with a password, `melody-watch` on a temporary
  folder, and a file written there appearing in the engine's library.
