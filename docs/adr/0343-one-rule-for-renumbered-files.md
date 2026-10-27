# ADR-0343: One rule for a file seen earlier, renumbered or not

## Status

Accepted, 2026-10-27. Completes ADR-0331; takes in ADR-0249's rule.

## Context

A tagger Apply that wrote tags and renamed 184 files on an sshfs mount
(`fuse.sshfs`, without `use_ino`) left 141 of them untouched with
"Destination-artifact executor requires one ready changed output path", and
the engine logged "undo copies not all moved: an undo copy has changed".

sshfs, like SMB mounted `noserverino`, numbers files on the client: a file
the kernel forgets and looks up again gets another inode number while
nothing about it changed. ADR-0331 let a check accept such a file -- same
device, size and modification time to the nanosecond, another inode -- but
only at some checks. The path preflight accepted it; the publication
executors behind it then compared the planned revision with `!=` and
refused. So did the write plan against the tagger's draft, each format
writer rereading its source, the prepared-copy check, the journal's
recovery and undo comparisons, and the move of undo copies, which used
ADR-0249's rename rule: on sshfs a rename keeps the number, so the rule
said "another file".

## Decision

- **One rule, by path: `core::same_file(raw_path, expected, observed)`.**
  Equal; or equal in all but the inode where the filesystem at `raw_path`
  (or, gone, the folder it was in) numbers its files itself -- SMB/CIFS,
  FUSE -- or where its renames renumber files (ADR-0249's probe). It
  replaces `same_file_after_rename`, which was the second half of it.
- **Every check of a file seen earlier and not held open uses it:** the
  write plan against the draft and the artwork plan against the inspected
  inventory -- both then expect the file as it is now, so the writers'
  own `expected == observed` invariants hold -- each writer's reread, the
  publication executors' preconditions, the apply's metadata match, the
  journal's recovery, rollback and undo comparisons, the metadata
  commit's carrier and backup checks, and undo copies being moved.
- **A file held open is checked by its descriptor** (`same_revision(…,
  descriptor)`), and a publication's journal now records the revision of
  the locked, open source rather than the one observed before it was
  opened.
- Elsewhere nothing changes: on a filesystem that keeps its numbers,
  another inode is another file.

## Consequences

- Tested in `file-publication`: a rename and a tags-plus-rename whose plan
  saw another inode number go ahead where the filesystem numbers its files
  (simulated by `simulate_unstable_inode_numbers_for_testing`) and are
  refused before any journal elsewhere; the test fails without the change.
  `core-primitives` covers the rule itself.
- Comparisons between two revisions recorded at the same moment, and those
  within one read, stay exact.
