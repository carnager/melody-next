# ADR-0321: The library knows its mounts; a file is as it was by its data

## Status

Accepted, 2026-10-25. Replaces ADR-0313's device numbers, both in deciding
whether a missing file is gone (point 2, "device moves") and in deciding
whether a file found is unchanged.

## Context

A refresh used device numbers twice:

- **Unchanged files.** A file was unchanged when its stored revision --
  device, inode, size, modification time -- matched exactly. A share mounted
  again after a reboot is often given another device number, and sshfs, or
  SMB mounted without server inode numbers, gives its files new inodes too:
  every file then read again, tags and all, on every reboot.
- **Missing files.** A file missing from a walk was removed only when a
  folder above it survived on the device it was stored with, to keep an
  unmounted share's empty mountpoint from emptying the library. ADR-0313
  added "device moves" for remounts, which only works while the scan sees
  files change device; a row stored under a device number later replaced
  stayed forever.

The kernel says what a folder is on: `/proc/self/mountinfo` names each
mount point, its filesystem and its source (`cifs //nas/multimedia`,
`fuse.sshfs host:/music`, `ext4 /dev/sda2`). macOS has no `/proc`;
there `getfsstat()` gives the same three (`apfs /dev/disk3s5`,
`smbfs //user@nas/music`). With no table at all, no file is ever
confirmed gone -- what macOS did until 2026-10-31.

## Decision

1. **A file is unchanged when its size and modification time are.** Device
   and inode are left out of that comparison, as MPD and other players do;
   a row takes what is observed now without the file being read.
2. **The library records the mounts its files were found on.** After a walk,
   the mount the folder is on and each mounted below it that holds files
   found by the walk are recorded (`local_library_mounts`: mount point,
   filesystem, source; schema 57). A recorded mount point that is no mount
   any more, where files were found all the same, is forgotten.
3. **A missing file is gone when the folder above it is on the mount it was
   found on** -- same mount point, filesystem and source, whatever device
   number the kernel gave it this time. A share not mounted leaves its
   mountpoint on another mount (the root filesystem): its files are kept,
   unavailable, and come back with it. A file under no recorded mount is
   kept.

Tests read another mount table through `MELODY_MOUNTINFO`.

## Consequences

- A reboot or remount costs a refresh what any refresh costs.
- Deleted files go after a remount however many refreshes ago the device
  number changed.
- Live watching of the library (as MPD's `auto_update`) is separate work.

## Verification

- `local-library`: a share not mounted keeps its files, unavailable; mounted
  again, they return without being read, and a file deleted meanwhile goes;
  a folder no longer a mount, its files there, loses a missing file; files
  stored with another device and inode are not read again and take the new
  ones; schema 57 round-trips.
