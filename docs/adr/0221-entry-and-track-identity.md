# ADR-0221: Entry identity and track identity

Status: Accepted

## Decision

Introduce two distinct identities and never let one serve both purposes.

**Entry identity** answers *which slot in this list*. It is an opaque
`core::StableId` assigned when an entry is inserted, carried as a column on
`list_items`, and discarded when the entry is removed. It distinguishes the
same track queued twice and is unaffected by reordering.

**Track identity** answers *which piece of music*. It is a SHA-256 over a fixed,
curated set of metadata fields. It survives re-encode and file movement, and
changes only when one of those fields is edited.

An entry carries a track identity as an opaque field. Neither is derived from
the other.

## Why two

A single identity cannot satisfy both. Addressing a queue position by track
identity fails on duplicates, which are ordinary: removing the first copy
silently renumbers the second, and a resume checkpoint then points at the wrong
occurrence. Addressing a stored rating by entry identity fails on the first
re-encode, because the entry is gone.

The previous design held playback position as `QPersistentModelIndex` — a live
pointer into a Qt view model. It cannot live in a core-owned playback service
and cannot be serialised, which is what blocks the unified engine's Phase 0.

## Entry identity

- `core::StableId`, 128 bits, already implemented with `random()`, `parse()`,
  `to_string()` and comparison, and already in use across the workspace.
- Assigned on insert. Reordering is an `UPDATE position`; identity is untouched.
- `list_items` gains an `entry_id` column, unique per document.

  **As implemented (migration 41), this is the column and a
  `(document_id, entry_id)` unique index only.** `PRIMARY KEY(document_id,
  position)` is unchanged, and `list_item_fields` still carries its composite
  foreign key on `(document_id, item_position)`. That was deliberate: identity
  semantics hold without it, because ordering lives in `position` and the
  repository writes each entry's identity alongside its row. Repointing the
  key and the foreign key is referential tidying with real migration risk and
  no behavioural gain, so it is available work rather than done work.
- Re-evaluating a dynamic list or saved search assigns new entry identities.
  Those genuinely are new entries; only the track identities they carry persist.
- Rejected alternative: keeping position as the key with a document revision
  counter. Every reorder then invalidates outstanding references, which over a
  protocol connection degenerates into retry loops when up-next is edited during
  playback.

## Track identity

Hash input, NUL-separated, prefixed with the scheme version `tk-track-1:`:

| Field | Fallback |
| --- | --- |
| `albumartist` | `artist`, then `Unknown Artist` |
| `album` | parent directory name |
| `discnumber` | `1` |
| `tracknumber` | `0` |
| `title` | filename stem |

Normalisation, applied to every text field in order: Unicode NFC, trim, collapse
internal whitespace runs, case fold.

### The inclusion rule

> A field belongs in the hash only if changing it means *this is a different
> track*. Everything else describes the track without identifying it.

Excluded under that rule, with the specific reason:

- **genre, comment, composer, artwork, ReplayGain tags, rating, play count** —
  descriptive, not identifying, and edited freely.
- **date / year** — remasters and retags churn it. It stays in the album hash
  only.
- **duration** — survives retagging but changes on re-encode, which is the case
  this design exists to survive.
- **track-level `artist`** — featuring credits are edited constantly and it
  differs from album artist across compilations.
- **path, container format, bitrate** — all change on re-encode.

**Editing an excluded field must be a pure write**: no identity change, no carry
transaction, nothing migrated. Retagging genre across ten thousand files touches
zero rating rows.

### Changing the field set later

The field set will eventually be wrong about something, so a scheme change must
be a supported operation rather than a database rewrite.

**Store the scheme version alongside every stored identity**, as its own column
— not only inside the hash input. A row must be able to say which scheme
produced it without the originating metadata being available.

**A scheme change is a bulk retag.** Recomputing every identity under
`tk-track-2:` is the same operation as retagging every file at once, so it
reuses the carry rule already required for ordinary edits: recompute from
current metadata, move ratings, history and list membership to the new
identity in one transaction. No separate migration engine.

**Migrate lazily, not big-bang.** A lookup resolves under the current scheme
first, then falls back through older schemes in order; on a fallback hit, the
row is re-keyed in place. A library of tens of thousands of files therefore
upgrades as it is used, and a mixed-version store is a legal steady state rather
than a window of inconsistency to be survived.

**Never drop a row that cannot be re-keyed.** Re-keying needs current metadata,
which requires the file to be reachable — a disconnected NAS or removed drive
makes that impossible. Those rows stay under their old scheme and re-key when
the file reappears. Losing a rating because a disk was unplugged during an
upgrade is not acceptable.

**Keep old scheme implementations.** Each is a pure function of a metadata
document; retaining them costs almost nothing and is what makes fallback
resolution possible. Deleting one strands every row still keyed under it.

### No MusicBrainz tiering

Preferring a MusicBrainz recording ID when present would flip identity scheme
the moment tagging adds one, for no gain the carry-on-write rule does not
already provide. One scheme, always the same fields.

## What implementation added

Three things the design above did not anticipate, all found by the compiler or
the test suite rather than by reasoning:

**Uniqueness is per document, not global.** A copied `ListItem` or
`LocalTrackRow` carries its source's identity, and `items = {item, item}` is
ordinary — duplicating a selection, dragging within a tab, or a test building a
list from one row. A global unique index rejected it outright. The index is
`(document_id, entry_id)`, and both `ListRepository::replace_all` and
`LocalListModel`'s insertion paths stamp a fresh identity for a repeat within
one list, because a copy is genuinely a new entry. Keeping the invariant at
those two boundaries means no caller has to remember it.

**Equality stays value equality.** `ListItem` and `LocalTrackRow` both had
defaulted or salient-tie `operator==` that would have absorbed the identity,
silently turning "same track" into "same track and same entry". Existing
comparisons depend on the former. Identity is excluded from both, and the
exclusion is commented where it might otherwise look like an oversight.

**Anything that rebuilds a row must carry the identity.** `applyMetadata` and
`applyProbeRows` replace a row wholesale with a freshly constructed one from a
background probe, preserving `logical_reference`, `selection`, `segment` and
`raw_path` — the fields that must outlive a probe. Identity was not among them,
so enrichment reassigned it and anything anchored to that row stopped resolving
the moment probing completed. Both now carry it; in a multi-entry probe the
first row keeps the identity it replaces and the rest are new entries. The
general rule: preserving identity belongs wherever a row is rebuilt rather than
mutated.

## Consequences

**Retagging an identifying field changes identity.** Every mutation that does so
must carry ratings, listening history and list membership to the new identity in
the same transaction as the write and the index update. This is the rule from
ADR-0220, and it is what makes a metadata-derived identity safe. It holds only
while every mutation path goes through the engine.

**Untagged files have a path dependency.** The `album` and `title` fallbacks are
derived from the filesystem, so moving or renaming an untagged file changes its
identity. This is deliberate: the alternative is every untagged file colliding
on a single hash. Tagged files have no path dependency.

**The same recording in different formats shares one identity.** A FLAC and an
MP3 of the same track hash identically, so ratings and play counts merge. This
is intended — the rating belongs to the music, not the file.

**Two existing schemes must be reconciled.** `rating_identity` hashes metadata;
`listening_track_key` hashes a source id plus stream selection. They have
opposite failure modes — one breaks on retag, the other on re-encode. Both
migrate onto this scheme, keyed by content identity.

## Verification

- Reorder, insert and remove against a list with the same track present twice;
  entry identities and the resume checkpoint stay attached to the correct
  occurrences.
- Re-encode a rated track with listening history; both survive under the new
  identity.
- Retag `genre` across a large selection; assert **zero** identity changes and
  zero rating rows touched. The failure mode is silent, so this guards against a
  later well-meaning addition to the hash.
- Same track tagged NFC and NFD hashes identically.
- A multi-disc release with a repeated title across discs yields distinct
  identities.
- A simulated `tk-track-2:` scheme migrates a mixed store: rows resolve under
  fallback, re-key on access, and rows whose files are unreachable survive
  unchanged and re-key once the files return.
- The playback service resolves position without `QPersistentModelIndex` and
  without constructing `BenchMainWindow`. Covered by `tests/playback-state`,
  which links `Trackknife::Audio` only and pulls in no Qt.
- A background probe completing does not change an entry's identity, and an
  anchor set before it still resolves afterwards.
