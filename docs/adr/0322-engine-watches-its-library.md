# ADR-0322: The engine watches its library's folders

## Status

Accepted, 2026-10-25.

## Context

The engine learned of changes in its music folders only by a Refresh, by
its own writes, or from melody-watch (ADR-0232) -- a separate program, made
for folders on another machine. MPD's `auto_update` shows what users expect:
a file copied into the music folder appears, one deleted goes, with nothing
pressed.

inotify reports what passes through the kernel it runs in. On the machine
holding the files that is every change; over an SMB, NFS or sshfs mount it
is the changes made through that mount on this machine, not the server's
own writes or another client's. melody-watch, run on the server, covers
those.

## Decision

1. **melodyd watches its library's folders** (Linux, where melody-watch
   builds), with melody-watch's tree watcher. Changes are taken together --
   after a second without one, or five seconds of them -- and only the files
   named are re-read: a file added, changed, moved or deleted; a folder gone
   drops what was under it. A lost queue (overflow) refreshes the library,
   which reads only what changed (ADR-0321). The folders are looked at again
   every ten seconds: one added, or a share mounted over its empty
   mountpoint, is watched from then on. Reaching the system's limit on
   watches is said once in the engine's log.
2. **A watcher's re-read is "changed only".** `catalogue.refresh` takes
   `changed_only`: a file indexed with its size and modification time as
   they are now is not read again. A file opened for writing and closed
   unwritten -- a tag reader's way, the engine's own reads among them --
   makes no work and no loop; melody-watch sends it too, so a change both
   report is read once.
3. **On by default; `watch = no`** in melodyd.conf (`--no-watch`) leaves the
   folders to Refresh. Melody's Engines page shows the setting for the
   engine chosen there -- this computer's or one elsewhere -- asked of it
   (`config.get`) and changed on it (`config.set`, then a restart).

## Consequences

- No Refresh after copying music into a folder on the engine's machine.
- On a share, the server's own changes still need melody-watch there, or a
  Refresh.
- One inotify watch per folder; a very large tree may need
  `fs.inotify.max_user_watches` raised.

## Verification

- `engine-methods`: a file added to a watched folder is indexed without a
  Refresh; one opened and closed unwritten changes nothing; one deleted goes.
- `local-library`: a changed-only refresh reads a file only when its size or
  modification time changed.
- `bench-main-window`: the Engines page shows the engine's own setting.
