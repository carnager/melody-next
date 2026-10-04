# Protocol levels

ADR-0260: `engine.info` answers `protocol` and `level`. The protocol is 1
for all of 1.x; the level goes up with every release that adds to it, and
each level's additions are listed here, newest first. A client compares the
engine's level with its own and says once when the engine is older.

## Protocol 1

### Level 3 (2026-10-04)

- `backups.location` and `backups.set_location {place: "engine"|"folder"|
  "beside", folder?}`, answering `{place, folder?, kept_in}`: where the
  engine keeps the files writes replace, moving every undo copy there when
  it changes (ADR-0266). An older engine keeps them beside each file.
- Undo and a failed write's rollback no longer need an atomic exchange, so
  both work on NFS and SMB (ADR-0266).

### Level 2 (2026-10-04)

- The `operations.undo` job: `{operations: [{kind: "metadata"|"publication",
  id}]}`, undone in the order given, answering `[{kind, id, from, to}]` or
  `[{kind, id, issue}]` for each (ADR-0263).
- A publication that writes a new file keeps the source it replaced, and
  backups are kept within the retention policy rather than released at the
  engine's start (ADR-0263): the journal ids `metadata.apply` and
  `preparation.apply` answer can be undone.
- `backups.retention` and `backups.set_retention {max_age_days?,
  max_writes?, max_gigabytes?}`: how long the files writes replaced are
  kept for undo; a lower limit lets go of the rest at once (ADR-0263).

### Level 1 (2026-10-03)

The protocol as it stood when levels began: everything through ADR-0259,
among the latest of it

- `list.describe {id, entries}`, `list.draft {of}`, `list.commit {id,
  force?}`, `list.from_query {query, name, words?}` and the `draft_of` and
  `draft_base` of list summaries (ADR-0259);
- `catalogue.select` and the selection fields of a list's continuation
  (ADR-0258);
- `engine.info` answering `level` and `release` (ADR-0260).

### Level 0

An engine that answers no `level`: anything from before 2026-10-03.
