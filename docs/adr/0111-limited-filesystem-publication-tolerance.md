# ADR-0111: Limited-filesystem publication tolerance

- Status: accepted
- Date: 2026-09-03
- Owners: Trackknife project
- Extends: ADR-0063 bounded publication apply, ADR-0105 conversion core,
  ADR-0083 trust over confirmation

## Context

Publishing to network and removable filesystems hit three walls, found by
probing a real NFSv4.2 NAS mount (and previously sshfs): `renameat2`
`RENAME_NOREPLACE` is rejected with EINVAL; extended-attribute support is
absent on some filesystems while NFS additionally exposes kernel-owned
names like `system.nfs4_acl` that refuse removal; and ownership changes
are refused under server-side identity mapping. A fourth, subtler wall:
publishing by hard link + unlink trips NFS silly-rename when the
publisher holds the file open, leaving a transient extra link that the
strict single-link topology verification rightly refuses.

## Decisions

- `core::publish_no_replace_at` is the shared no-replace publish:
  `renameat2(RENAME_NOREPLACE)` first; on flag rejection a plain rename
  immediately follows — safe because the VFS resolves the target before
  the filesystem sees the flag, so an occupied target already reported
  EEXIST; on ENOSYS (no renameat2 at all) an explicit existence check
  precedes the rename. A hard-link rung was deliberately rejected: it is
  fully atomic but breaks under NFS silly-rename with open publishers.
  The converter and the publication executor both use the ladder.
- Extended-attribute preservation manages the `user.` namespace only;
  `system.`/`security.`/`trusted.` names are kernel- or filesystem-owned
  representations (NFS ACLs, SELinux labels) the destination manages
  itself. A filesystem reporting ENOTSUP lists as empty-and-unsupported:
  publication proceeds, verification compares only where both sides
  support xattrs, and when the source actually carried user attributes a
  note reports the loss instead of an error. Zero-xattr sources publish
  with no note at all — the original sshfs failure was exactly this
  over-strict case.
- Ownership preservation (`fchown`) tolerates EPERM/ENOTSUP with a note;
  mode preservation tolerates ENOTSUP. Verification and recovery enforce
  only what was achievable; recovery no longer compares ownership at all,
  as host identity mapping is not content.
- Notes travel on `FilePublicationCommitResult::notes`. An all-success
  run with notes closes as any success does and says them once in the
  window's status bar; each file's note goes to the `--debug` log.
  (It first opened an "Updated with notes" window, but on an NFS share
  the ownership note comes with every file of every save and leaves
  nothing to do, so the window only taught closing it unread.) Tag commits
  (source and prepared copy share one filesystem) degrade silently —
  a filesystem without xattrs has none to lose on either side.

## Verification

Probed and proven on the real NAS (`tauron:/mnt/MEDIA/MEDIA`, NFS 4.2):
xattrs supported, `renameat2` flags rejected, `link` works, `fchown`
refused even to the owning uid. The full audio-convert suite passes with
its temporaries on the mount, and a real local→NAS move publication
committed end to end — bytes verified, `user.` xattr preserved,
ownership note surfaced, source removed only after verification.
