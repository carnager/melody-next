# ADR-0331: A file renumbered by the client is the same file

## Status

Accepted, 2026-10-26.

## Context

Before a file is written or moved, the engine checks it is still the one
the plan saw: its revision -- device, inode, size, modification time to the
nanosecond. A tags-and-move Apply of 216 files on an SMB share committed
184 and refused the last 32: "Source revision changed after path
planning", though nothing had written them.

An SMB share mounted with `noserverino` (and some servers give no inode
numbers at all), and FUSE filesystems such as sshfs, number their files on
the client: a file the kernel forgets and looks up again is given another
inode number. A long Apply over the network is minutes; the files planned
first and reached last are the ones renumbered meanwhile. The share's
library rows had already shown it: small inode numbers handed out in turn
(ADR-0321).

## Decision

- On a filesystem whose inode numbers are the client's -- SMB/CIFS, FUSE --
  a file is still the one expected when its revision is the same but for
  the inode: device, size and modification time to the nanosecond as they
  were. Elsewhere a new inode is another file, as before.
- Asked of the open file itself (`fstatfs`), at each check: the write
  plan's, the path preflight's, the publication's, the journal's and
  recovery's.

## Consequences

- On such a share a file replaced by another of the same size and the same
  modification time to the nanosecond would pass for it: not a case that
  happens by accident.

## Verification

- `core-primitives`: the rule told unstable and stable inodes; a file on
  the local filesystem keeps strict identity.
