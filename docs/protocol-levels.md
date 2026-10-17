# Protocol levels

ADR-0260: `engine.info` answers `protocol` and `level`. The protocol is 1
for all of 1.x; the level goes up with every release that adds to it, and
each level's additions are listed here, newest first. A client compares the
engine's level with its own and says once when the engine is older.

## Protocol 1

### Level 17 (2026-10-17)

Conversion where the files are, as `melody-cli convert` asks it.

- `convert.presets {}` -> `{presets: [{id, name, extension, available,
  detail}]}`: what the engine's FFmpeg can make.
- The `convert.apply` job `{paths: [file or folder], to, layout?, preset?,
  gain?, artwork?, dry_run?}`: the files and the audio files in the folders
  converted beneath `to` with the window's converter -- named by the
  engine's naming layout of that name, else kept below their music folder --
  tags and cover carried and verified, one there already passed over;
  reporting `{completed, total}`, finishing with `{result: {files: [{source,
  target, converted, error}], passed, problems}}`. All paths are encoded
  raw paths, the engine's.

### Level 16 (2026-10-17)

ADR-0289: undo copies only for a tagger, while it is open.

- `metadata.apply`, `artwork.apply` and `preparation.apply` take `keep_undo`
  (default false): kept, what the write replaced can be undone; without it,
  it is let go of once written. An older engine keeps everything within
  its limits.
- `backups.release {operations: [{kind, id}]}` -> `{released, error}`: the
  undo copies of those writes let go of; one with none kept is passed over.
- `backups.retention` and `backups.set_retention` are gone; a write is no
  longer kept within limits.

### Level 15 (2026-10-16)

ADR-0286: tabs are Melody's; only saved playlists live on the engine.

- `list.save` makes saved lists (`kind` defaults to `saved`); `kind:
  "working"`, there and in `list.edit`, is refused as `unsupported`.
  `list.from_query` makes a saved list. `list.draft` and `list.commit` are
  gone (no client used them).
- Schema 56 sets the working lists and drafts there were aside:
  `list.retired {}` answers `{lists}` as `list.all` does, `list.retired_get
  {id}` one with its `items`, `list.retired_drop {ids}` drops them once a
  client has dealt with them.
- `catalogue.describe {paths}` (at most 2,000): `{items: [{path, library}]}`,
  `library` as `list.describe` gives it, null for a file the library does not
  index.
- `catalogue.resolve_moves {items: [{path, revision}]}` (at most 2,000):
  `{items: [{path, moved}]}`, `moved` `{path, revision}` for a file this
  engine moved since it was at that path and revision, else null.
- `playback.edit_queue {list?, edits}`: the queue changed in place with
  `list.edit`'s edits -- a long queue sent in parts after
  `playback.replace_queue`, and a tab's edits after. An edit naming an entry
  the queue no longer holds leaves it out; an insert after one goes at the
  end. Answers `{queue_revision}`; refused as `conflict` when the queue plays
  another `list`.

### Level 14 (2026-10-16)

- `engine.info` says whether a service manager runs the engine: `service`,
  true when systemd handed it its socket or runs its unit (ADR-0286). An
  older engine leaves it out, which reads as false.
- The engine's own `melodyd.conf` (ADR-0286): `config.get {}` answers `{path,
  exists, settings, secrets, engines: [{name, settings, secrets}],
  from_flags}` -- a password is never sent, only named among `secrets` --
  and `config.set {changes: [{key, value, engine?}]}` sets each, a null
  `value` taking it out, all or none, checked as melodyd reads the file and
  written with the rest of it kept: `{path, restart, overridden}`.
  `engine.restart {}` answers, then stops the engine for its service manager
  to start again with what the file says; `engine.stop {}` stops it for good,
  exiting with 64, which melodyd.service does not restart after.

### Level 13 (2026-10-15)

- `playback.now_playing` answers what plays as fields, for a client that lays
  it out itself (Home Assistant's media card): `{track: {entry, instance,
  path, title, artist, album, album_artist, date, track_number, disc_number,
  genre, duration_ms, rating}}`, a field not known being null, or `{track:
  null}` when nothing plays. From the engine's library, or the queue entry for
  a file it does not index -- as `playback.format` reads them. Asked when the
  playing `entry` or `instance` changes; its cover is `catalogue.artwork
  {path}`. An older engine does not know the method.

### Level 12 (2026-10-14)

- A track's description -- `list.describe`'s `library` -- says when it, and
  its album's newest track, came into the library: `added`, `album_added`
  (Unix seconds, 0 when not known), for a list to show and sort by
  (ADR-0283). An older engine says neither, and they read as not known.

### Level 11 (2026-10-14)

- The `catalogue.listened` event `{path}`: a listen was recorded for that
  file, so play counts and last-played times shown of it follow (ADR-0282).
  An older engine records them without saying so.

### Level 10 (2026-10-11)

- A file-work result names what was written, not its tags: a commit
  (`metadata.apply`, artwork, publication, undo) no longer carries its
  `document`, nor a publication its `published_metadata`. Every file's tags
  made the result of a large Apply megabytes, past the 4 MiB a connection
  holds for its events, and the client was let go ("the engine closed the
  connection"). A client wanting the tags reads the files (ADR-0278). An
  older client expecting them reads none and keeps the rows it had.

### Level 9 (2026-10-10)

- `catalogue.lyrics {path}` answers a track's lyrics, read where the files
  are: `{lyrics: {source, synced, lines: [{time_ms?, text}]}}`, or
  `{lyrics: null}` when it has none (ADR-0276). An older engine does not
  know the method.

### Level 8 (2026-10-06)

- `agent.register` takes `report`: what the agent plays now, as its
  `audition.changed` reports say it. The same `instance` back after a drop,
  still on what the engine plays (or what it armed), is taken as it is --
  nothing loaded again (ADR-0274). An older engine takes it up where it
  dropped.

### Level 7 (2026-10-05)

- A view's `catalogue.query` that names a file (`path`) answers the way to
  it -- its node at each level below `view_path` -- with `located: true`,
  for a client to locate a file in the view shown (ADR-0273). An older
  engine answers the node's children and no `located`.

### Level 6 (2026-10-05)

- The protocol as a WebSocket at `/protocol` on the stream port (`--http`),
  for a client behind an HTTP proxy: one message in each text message, both
  ways, the password asked as on TCP; refused where the engine has no TCP
  listener with a password (ADR-0271).

### Level 5 (2026-10-05)

- Playback states carry `requests_revision`, which moves whenever Up Next
  does -- added, reordered, played, cleared -- and `playback.requests
  {details: true}` answers the entries themselves with that revision, not
  only their identities (ADR-0269). Without `details` it answers as before.

### Level 4 (2026-10-04)

- The `catalogue.changed` event `{paths, albums, everything}`: what changed
  in the library -- the paths re-read or dropped, the album keys they
  touched (a cover written names the albums of its folder), or after a scan
  or a change of folders, everything (ADR-0268). An older engine says
  nothing; its clients see changes when they ask again.
- `files.missing {paths}` answering `{missing}`: which of these files are
  not there, looked for on the engine's machine (ADR-0268).

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
