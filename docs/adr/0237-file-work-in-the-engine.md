# ADR-0237: File work moves into the engine

Status: Accepted (2026-09-27)

Reopens the 2026-09-23 decision that file work stays in Trackknife, by path
over a mount. Continues ADR-0220 ("one engine owning catalogue, mutation and
playback") and ADR-0234 (engines are connections).

## Context

Engines are now first-class: each owns its library, lists and playback, and
any client -- Trackknife, the phone, `melody-cli`, another engine -- reaches
any engine the same way. File work is the exception. Tagging, artwork,
MusicBrainz matching, ReplayGain, moves and conversion are done by Trackknife
itself, on files it can reach: this computer's, or an engine's through a
mount configured per engine (its music folder, and where that is mounted
here).

That leaves three gaps:

- **Only Trackknife on a machine with the mount can do it.** The phone, the
  CLI and any computer without the mount cannot tag, scan or rate into tags.
- **The client reaches the files only sometimes; the engine always does.**
  The engine indexes and streams them, so it has them by definition. A client
  needs a mount that exists, is up, and maps paths correctly. This is the
  reason for the change. It is not a speed argument: an engine on a home
  server reading a NAS goes over the network just as a desktop does.
- **Consistency is after the fact.** Trackknife writes, then tells each engine
  (`list.relocate`, refreshes); the index, lists, ratings and history catch
  up afterwards instead of changing in the same step.

The 2026-09-23 decision set remote file operations aside because they felt
"not in control" and brought modes. That is the constraint on this design,
not an argument against it.

## Decision

**The engine does the file work on the files it serves.** Trackknife keeps
the tagger, the ReplayGain view and the converter as its user interface, and
they become clients: they ask the engine for a plan, show it, and commit it.
This computer's engine does the same for files outside any library (opened
from Folders), since it can reach every local path. In the end state
Trackknife has no file-writing path of its own -- one implementation, not two.

**The UI and the workflow do not change. This is a requirement, not a goal.**
The tagger, the ReplayGain view, the converter, their previews, shortcuts,
undo and every step of how they are used stay exactly as they are today --
the user's words: "I love the workflow and UI like it is now, don't change
it". What moves is only where the work runs. No mode, no "remote" variant of
a dialog, no new confirmation, no extra step: the engine a tab's files belong
to is where the work runs, as it is already where they play.

Every stage below is accepted only if a user doing the same task sees the
same windows, the same previews and the same results as before it. A visible
difference is a defect in the stage, not a consequence of it; the existing
window tests for these workflows keep passing unchanged, and are the check.

**Previews are the engine's, shown by the client.**

- `plan.create {kind, targets, params}` -- the engine reads the files, builds
  the complete plan (every file, every change, every conflict) and answers
  with a plan id and the preview document. Nothing is written.
- `plan.commit {plan_id}` -- runs the plan as a job (`job.progress`,
  `job.finished`, `job.cancel`, as `catalogue.scan` does). The engine checks
  each file's revision against the one the plan was built from and refuses a
  file that changed; the preview is the contract.
- `plan.discard {plan_id}` -- or it expires.
- The recovery journal and undo live in the engine's state directory, beside
  the files' index, and survive the client closing.

The index, lists, ratings and history change in the same step as the write,
on the engine that did it.

**As built, stage 1:** the plan is built by the client, from what the
engine reads (`metadata.read`), with the metadata library's own planner --
the one the dialogs already use -- rather than by a `plan.create` on the
engine. The engine holds it to the same contract: `metadata.apply` refuses
a plan with blocking issues, and re-checks every file's revision before
writing it. The dialogs keep their code; only where they read, measure and
write moves (`FileWorkTools`, `MetadataFileAccess`). `plan.create` stays
available if a client without the planner ever needs it.

## Stages

Each leaves the application working and Trackknife's own path in place for
what has not moved yet, and each is held to the rule above: nothing a user
sees changes.

1. **ReplayGain scans** in the engine: a job with a result preview and little
   UI; results written to tags, CUE sheets or sidecars as today. **Done for the
   ReplayGain dialog** (the track menu's ReplayGain…): `metadata.read`,
   the `loudness.scan` and `metadata.apply` jobs, startup recovery and
   `metadata.interrupted`, and `RemoteFileWork` as the client's end. The
   Properties window's scan button follows with stage 3, the tagger.
2. **Ratings into tags**, as an engine job over a selection or the library
   (the tag mapping is its own decision). **Done** (decided 2026-09-27:
   ratings are for every client, tags are an option): ratings stay in each
   engine's library, where every client reads and sets them. With the option
   on (`ratings.set_tags`; Trackknife's Settings → Library hands it to every
   engine it reaches), the engine also writes each track rating into its
   files as `FMPS_RATING` (the rating over ten, 0.1-1.0; unrated removes it),
   through the tagger's own plan and journal, on a thread of its own. Turning
   it on writes every rated track once; turning it off leaves the files as
   they are. Album ratings are not written: no player reads them from tags.
   Where each format's players read it (decided 2026-09-28): `FMPS_RATING`
   everywhere, spelled `FMPS_Rating` in ID3v2 `TXXX` and the MP4 freeform
   atom as the FMPS specification has it; on MP3 also `POPM` as Windows
   Media Player's owner, with its whole-star bytes and MediaMonkey's and
   MusicBee's half stars between (13, 1, 54, 64, 118, 128, 186, 196, 242,
   255 for 1-10), so all ten levels survive there and foobar2000 and
   Windows show the nearest star.
   **Import** (wanted, 2026-09-27): whenever the library reads a file, a
   rating in it -- MP3 `POPM` (any owner; the ten bytes exactly, others by
   the ranges players share), else `FMPS_RATING`, else a plain `RATING` on
   the scale chosen in Settings (off by default; 1-5, 0-10 or 0-100: it has
   no agreed one) -- becomes the track's when it differs from what the
   library read there before, or the track is unrated. A rating the library has over a tag it already saw
   stays, so a cleared rating is not brought back by an old tag. A library
   read before this is caught up once, at the engine's start, from the tags
   it indexed.
3. **Tag edits** -- the spreadsheet tagger on the plan/commit protocol.
   **Done:** Properties captures, probes (`media.probe`), scans, plans and
   applies through the engine; MusicBrainz, the Cover Art Archive and
   AcoustID are looked up by the engine (`musicbrainz.fetch`,
   `acoustid.fingerprint`, `acoustid.lookup`), with the AcoustID key kept
   there.
4. **Artwork** -- bytes to and from the engine. **Done:** the artwork
   section shows, reviews and writes pictures through the engine
   (`artwork.inventory`, `.image_file`, `.image_bytes`, `.destination`, the
   `artwork.apply` job), and tag plans carry artwork to `metadata.apply`.
   An image of the client's -- picked on its disk, downloaded, or resized --
   is handed over with `artwork.stage` into the engine's staging folder,
   named by content, and the plan names that copy. Resizing stays the
   client's Qt code, so a resized cover is the same bytes either way.
5. **Moves and renames**, with lists, queue and history following in the
   engine's own transaction. Choosing a destination uses the folder chooser
   as it is today, listing the engine's folders instead of this computer's
   -- not a new dialog. **Done:** Properties builds the path plan from the
   saved layout and destination as before; the engine checks it against its
   filesystem (the `paths.preflight` job) and publishes the reviewed
   preparation -- tags, paths, or both at once (`preparation.apply`). Each
   move is followed in the same commit: the workspace's record and library
   row (so ratings and history stay with the file, even one no list holds),
   the library index, every engine list (`list.changed`) and the player's
   queue. Startup recovery covers interrupted moves, and
   `metadata.interrupted` lists those it could not settle with their target.
   A move made by an engine elsewhere is followed in this computer's lists
   at its mount's path. Destinations are the engine's own (below), chosen
   among its folders.
6. **Conversion stays in Trackknife** (decided 2026-09-27). It makes new
   files where the user wants them -- this computer's disk, a stick, a
   phone -- and changes nothing in the library, so it belongs where the
   user sits. For files of an engine elsewhere, Trackknife fetches the
   originals from that engine, as the phone downloads albums (ADR-0230),
   and converts them here: no mount needed. **Done:** a remote tab's files
   reachable through the mount convert as before; the others are fetched
   by `RemoteFileWork::download_original` (a `streams.ticket` for the file
   as it is, then the stream port) into Trackknife's cache folder when
   Convert starts, converted from there and deleted. The preview plans them
   from what the engine knows, so it shows before anything is fetched.
7. Trackknife's own path for writing library files goes; conversion, which
   writes only its own new files, stays. **Done:** tags, ReplayGain,
   artwork, moves and renames are written only by the engine holding the
   files; with none that does file work, Properties and ReplayGain say so in
   the status bar instead of opening. Trackknife no longer recovers a
   journal of its own at startup: each engine recovers its own when it
   starts (this computer's shares Trackknife's database) and reports what
   it recovered and what it could not, which Trackknife shows as before.
   Its own MusicBrainz fetching and the editing of a remote tab's files
   through the mount went with it.

## Naming layouts are global; move destinations are the engine's (decided 2026-09-28)

A move destination is a folder on one machine. A naming layout is a rule
that is the same everywhere -- and what any client that moves files must
name them with, the CLI and the phone too, not only Trackknife.

- **Naming layouts are global presets.** They are made and edited in
  Trackknife, as today, and every engine holds a copy: Trackknife hands its
  whole set to each engine when it connects and whenever one changes (as
  the AcoustID key and rating options are handed over), replacing the
  engine's. Clients that move files through an engine name them with the
  engine's copy, so the CLI and phone have them with Trackknife closed;
  they use them and do not edit them, so the copies never need merging.
- **Move destinations are the engine's.** Each engine keeps its own, in its
  workspace beside its lists (`destinations.*`); every client reads and
  saves them there. This computer's engine shares Trackknife's database, so
  the ones saved today are already its own.
- Whatever shows or picks destinations names the engine. Properties offers
  only those of the engine the selected tracks belong to -- never a mixed
  list; the window that manages them is titled for it ("Move destinations
  on gemenon"); choosing a destination folder browses that engine's folders
  (this computer's with the usual file dialog, an engine elsewhere's
  through `folders.list`).
- Conversion runs here, so it uses this computer's destinations.
- An engine elsewhere starts with no destinations; its window offers to
  copy this computer's that lie under a configured mount, translated to
  that engine's paths.

## Decided with it (2026-09-27)

- **Write access is the engine's password.** A client that may control an
  engine may change its files; there is no read-only setting or second
  password. Every TCP peer already needs the password (ADR-0223).
- **An engine may write wherever its process can.** Not only inside its
  library folders: it tags what it can reach, as Trackknife does today.
- **MusicBrainz and AcoustID lookups run in the engine,** next to the files,
  with the API keys kept there.
- **Proposal:** the phone and the CLI get file operations only after stage 3,
  and read-only views of plans before that.

## Consequences

- Any client can do file work on any engine it can reach, with no mount.
- The per-engine mount settings ("music folder as it sees it", "reachable
  here at") no longer carry file work. They remain for what is done here
  with an engine's files: converting ones reachable through the mount
  without downloading them, following an engine's moves in this computer's
  lists, and offering this computer's destinations to copy.
- The protocol grows a plan/commit surface, which is the bulk of the work.
- Choosing folders on an engine's machine needs the engine to list them; the
  chooser itself stays the one the user knows.
