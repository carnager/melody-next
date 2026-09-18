# ADR-0179: Track and album ratings across MPD, Melody, and the local library

## Status

Accepted, 2026-09-18.

## Context

The roadmap's listening-history section and
`docs/playback-library-conversion.md` already commit to per-track statistics
that bind to stable identity rather than paths and never enter audio tags
without explicit opt-in. Ratings are the first such attribute users asked for,
and they exist in three authorities with different capabilities:

- Stock MPD stores per-song stickers when the server has a configured
  `sticker_file`. Established clients (Cantata, myMPD, the shared
  `mpd-stickers` documentation) use a sticker named `rating` with an integer
  value 0-10, i.e. a 5-star scale with half-star steps.
- Melody has no sticker support but ships a native rating extension:
  `rate <songid> <0-10>` keyed by the `X-SongId` database id from song
  listings, `albumrate <albumartist> <album> <date> <0-10>`, the read commands
  `getrating`/`getalbumrating`, an `X-Rating` line on every song listing, a
  `rating` idle subsystem, and content-hash storage
  (sha256 of albumartist/album/title/tracknumber for tracks and of
  albumartist/album/date for albums) so ratings survive path changes and
  database rebuilds.
- The local workstation has no rating storage at all, and no playback
  statistics precedent yet.

## Decision

One rating vocabulary everywhere: an integer 0-10, where 0 means unrated and
removes the stored value. UI presents it as five stars; whole stars are the
editing granularity, half-star values received from other clients display as
half stars and are preserved until the user re-rates.

**MPD authority, stock servers.** Track ratings use the interoperable song
sticker `rating`. Support is capability-gated on the advertised `sticker`
command. Ratings for display load in bulk through
`sticker find song "" rating` and refresh on the sticker idle event; rating a
selection issues one sticker mutation per track (0 deletes the sticker).

**MPD authority, Melody servers.** When the server advertises `getrating`
(probed from the same `commands` list as every other extension), the Melody
native commands are used instead of stickers: `rate` with the track's
`X-SongId`, and `albumrate`/`getalbumrating` with the album's literal
AlbumArtist/Album/Date identity. Per-track values for display come from the
`X-Rating` lines already present in every listing. Album rating appears on
album nodes of the server library. Melody's custom `rating` idle subsystem is
not representable through libmpdclient's idle enum; listings therefore refresh
ratings whenever they reload, which is recorded as a known limit rather than
worked around with a raw idle connection.

**Local authority.** A new `local_ratings` table mirrors Melody's storage
exactly: `hash TEXT PRIMARY KEY, type ('track'|'album'), rating INTEGER 0-10,
updated_at`, with the same sha256 identity hashes. Identity comes from tags,
not paths, so ratings survive rescans, renames, and moves with no relocation
bookkeeping, and a future Melody synchronization can match records one to one.
The library index stores each track's precomputed track and album rating
hashes (migration 35) and joins ratings into track rows and album aggregates;
files outside the library rate through the same hashes computed from their
open documents. Retagging the identity fields orphans a rating, as it does in
Melody; the rating follows the content identity, not the file.

**Tags remain untouched.** No rating is read from or written to file tags in
this change. The recorded future path is an explicit opt-in operation through
the previewed metadata write-plan workflow, mapping stars to the established
conventions per container (ID3v2 `POPM` with the common 1/64/128/196/255
mapping, Vorbis-family `RATING`), with the database remaining authoritative.
A Vorbis `RATING` comment that already exists in a file keeps flowing through
the generic field pipeline unchanged; it is display metadata, not the rating
store.

**Presentation.** Track views in both authorities gain a Rating column
(default hidden for MPD servers without rating support). The shared track
context menu gains a Rate submenu (Unrate, 1-5 stars) enabled per authority
capability. Saved track-view layouts predating a newly registered column now
migrate by appending the new column with its default spec instead of being
rejected wholesale.

## Consequences

The 0-10 scale is bit-compatible with both the sticker convention and Melody,
so no conversion layer exists anywhere. Choosing Melody's content-hash
identity for local storage makes ratings the template that play counts and
other statistics will follow, and keeps a future local-to-Melody rating sync a
pure key join. The cost is that identity-tag edits detach ratings; that
matches Melody's semantics and is preferred over path binding, which breaks on
every move. Stock MPD album ratings do not exist (stickers of type album are
not portable across clients); album rating is Melody and local only.
