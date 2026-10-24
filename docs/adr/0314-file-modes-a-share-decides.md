# ADR-0314: File modes a share decides

## Status

Accepted, 2026-10-24. Changes how every rewrite of a file -- tags, artwork,
a move's publication, folder images, undo copies -- gives the new file the
original's mode.

## Context

A file is rewritten by preparing a copy beside it, giving the copy the
original's permission bits, and putting it in the original's place. On a
network share the second step can be refused: NFS with its owners squashed,
or an SMB share that sets modes from its own options, answers `chmod` with
EPERM or EACCES -- the file may be written, but its mode is the share's to
decide. Every write there failed with "restoring prepared FLAC permissions
failed: Permission denied", untouched, on one album of a share while another
album of the same share, its files owned differently, was written.

## Decision

1. **A mode the filesystem refuses to set is not a failure.** Where setting
   the original's mode on the prepared copy fails with EPERM, EACCES or
   ENOTSUP, the write goes on: the copy keeps the mode the share gave it, as
   every file made there does. Any other failure still fails the write.
2. **One helper for every rewrite**, `core::keep_mode`, used by the tag
   writers, artwork, file publication, folder images, metadata commit and
   undo copies. Melody's own settings files, whose mode keeps a password
   private, still fail when it cannot be set.

## Verification

- `core`: a mode set on a file of one's own; a missing file a failure; a
  file not one's own -- refused as a share refuses it -- no failure;
  EPERM and ENOTSUP taken as refused, ENOENT and EIO not.
