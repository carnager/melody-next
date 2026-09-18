# Metadata, tagging, artwork, and file operations

## Application scope

This document specifies Trackbench's Local Queue authority. Per ADR-0058, all
tagging, artwork, MusicBrainz, file rename/move/copy, ReplayGain, and conversion
work remains structurally limited to local-file rows. Trackbench's MPD Queue
authority and the compatibility Trackknife shell only read metadata reported by
the server and cannot reach local file operations.

## Publication while playing or editing

**Trackknife decision (ADR-0121):** Saving tags or artwork updates queue rows
without losing the playing occurrence or automatic advancement. Embedding
artwork within Properties advances the pending tag draft's revision through
that verified commit, preserving MusicBrainz edits and undo/redo. Apply still
rejects unrelated file changes through fresh revision checks.

## Reviewing fields and identifying untagged albums

The Edit tags file selector shows full paths with readable Unicode filenames.
Invalid filename bytes and control characters use lossless escapes; display
formatting never changes the raw paths used for file operations.

**Trackknife decision (ADR-0157):** Properties can filter field names and show
only fields changed in the selected files. **Show files** collapses the file
selector while preserving its scope. Bold field names supplement draft colors;
the review bar reports visible/changed counts. Filters only affect presentation:
**Apply includes hidden edits**. Adding a field clears filters to reveal it.

**Trackknife decision (ADR-0158):** Identify works with manually entered artist
or album text even when files have no tags. Search no longer requires the
selected file count to equal a release's track count. **Match this version…**
opens two aligned panes: local filenames on the left and MusicBrainz tracks
on the right (ADR-0162). Drag files within the left pane or use **Move file
up/down** / **Alt+Up/Down**; MusicBrainz rows stay in album order. Each left
row pairs with the right row beside it. Arrow/track-number markers, paired-row
tint, linked selection, and synchronized scrolling make that relationship
visible. Full local paths are available in tooltips. **Match by filename** pairs files in
natural filename order; **Reset file order** uses original selection order.
**Leave unmatched** moves a file below the album and leaves an empty slot.
You can move empty slots to account for missing tracks. Extra unmatched files
receive no tags. Suggested pairings require review, especially for untagged
files whose remaining slots start in file order. **Stage matches** confirms the
shown mapping and creates an undoable draft; **Apply** writes it later.

Throttled MusicBrainz requests retry twice at most, honoring server cooldowns.
Trackknife keeps its own identifying User-Agent and normal request pacing.
See ADR-0158 for limits, evidence, and the API policy.

## Metadata is a typed, multi-source model

The core must not reduce tags to `Map<String, String>`. A useful logical model
is:

```text
MetadataDocument
  fields: ordered collection of Field
  pictures: ordered collection of Picture
  chapters/cue data: optional structured data
  loudness: optional LoudnessRecord
  unknown/native objects: retained by format adapter where possible

Field
  explicit semantic name, when the adapter recognizes one
  exact native property/frame/atom identity
  ordered values: [string]
  language/description qualifiers where the format supports them
  provenance: cached snapshot | annotation | embedded | stream | segment | sidecar
```

Semantic field lookup for title formatting is case-insensitive. Freeform native
fields retain their format-defined identity and are displayed, editable, and
removable without being aliased to a similar semantic field. Writes should
preserve unknown native data and the original representation when it is safe.

Technical properties—codec, duration, sample rate, channels, bit depth,
encoder, container, bitrate—are read-only decoder/container information, not
ordinary editable tags. Playback statistics and Trackbench-only annotations
also have distinct provenance.

**Trackbench decision (ADRs 0033, 0043–0047, and 0066, M5 baseline):** the
Qt-free document and conservative TagLib property reader are implemented.
ADR-0066 supersedes separator-derived aliasing: only an explicit adapter table
creates a semantic field, while every unrecognized exposed property retains a
case-folded native identity without losing separators or punctuation. Reads
preserve ordered repeated values, project
the initial MusicBrainz identity set, inventory unsupported native objects, and
carry a raw-path filesystem revision. Native FLAC now has an independent
Vorbis-comment text writer that creates only a distinct prepared copy, rejects
unrepresentable mappings, rereads all text, and byte-verifies preserved FLAC
blocks and compressed audio on every operation. Its headless executor now adds
per-source serialization, locked revalidation, a durable journal, preserved
filesystem metadata, retained exact backup, atomic publication, reread,
dependent-state commit, rollback, and conservative startup recovery. Startup
now presents recovery and ambiguous evidence, while retained backups support
verified crash-recoverable undo, explicit release, and bounded maintenance.
ADR-0047 exposes that complete path through an explicit two-worker cancellable
Apply job with per-source results and fresh-preview retry. Other formats and
picture mutation imply no write capability.

## Format adapters

Each adapter publishes independent capabilities: probe, metadata read, metadata
write, artwork read/write, chapter/cue handling, loudness read/write, padding
optimization, and preservation guarantees. An adapter must fail explicitly when
a requested value cannot be represented.

Important tag families include:

- ID3v1/ID3v2 for MP3 and ID3 chunks in formats such as WAV/AIFF;
- Vorbis comments and FLAC metadata blocks;
- Opus comments plus the Opus output-gain field;
- APEv2 for APE/WavPack/Musepack and optional uses elsewhere;
- MP4 atoms/freeform `----` metadata;
- ASF/WMA attributes;
- Matroska tags, attachments, and chapters;
- RIFF/BWF/INFO and AIFF chunks;
- cue-sheet metadata;
- sidecar metadata where embedding is impossible or deliberately disabled.

Do not assume the same logical field maps identically everywhere. For example,
foobar2000 historically maps common ID3 fields to native frames (`TITLE` to
`TIT2`, `ARTIST` to `TPE1`, `ALBUM ARTIST` to `TPE2`, track/total track to a
combined `TRCK`, disc/total discs to `TPOS`) and uses user-defined frames for
many non-native names. A `TXXX` description that resembles a conventional
field is still a separate native object; no spelling heuristic may map it to
`TPE2` or another standard frame. Trackbench needs a documented mapping table
and typed native identity per adapter, plus interoperability fixtures with
other taggers/players.

### Native FLAC text mapping (`taglib-flac-v1`)

ADR-0043 qualifies only native FLAC Vorbis-comment text. Existing fields retain
TagLib's exposed uppercase key. The initial conventional mapping for newly
added fields is:

| Trackbench field | FLAC key |
| --- | --- |
| Title, Artist, Album Artist, Album | `TITLE`, `ARTIST`, `ALBUMARTIST`, `ALBUM` |
| Date, Original Date | `DATE`, `ORIGINALDATE` |
| Track Number, Total Tracks | `TRACKNUMBER`, `TOTALTRACKS` |
| Disc Number, Total Discs | `DISCNUMBER`, `TOTALDISCS` |
| Genre, Composer, Performer, Conductor, Lyricist | `GENRE`, `COMPOSER`, `PERFORMER`, `CONDUCTOR`, `LYRICIST` |
| Label, Catalog Number, Barcode, ISRC | `LABEL`, `CATALOGNUMBER`, `BARCODE`, `ISRC` |
| Comment, Grouping, Copyright, BPM, Compilation | `COMMENT`, `GROUPING`, `COPYRIGHT`, `BPM`, `COMPILATION` |
| Subtitle, Version, Language, Media, Encoder | `SUBTITLE`, `VERSION`, `LANGUAGE`, `MEDIA`, `ENCODER` |
| Artist Sort, Album Artist Sort | `ARTISTSORT`, `ALBUMARTISTSORT` |
| Artists, Album Artists | `ARTISTS`, `ALBUMARTISTS` |
| MusicBrainz Artist Id | `MUSICBRAINZ_ARTISTID` |
| MusicBrainz Album Artist Id | `MUSICBRAINZ_ALBUMARTISTID` |
| MusicBrainz Track Id | `MUSICBRAINZ_TRACKID` |
| MusicBrainz Release Track Id | `MUSICBRAINZ_RELEASETRACKID` |
| MusicBrainz Album Id | `MUSICBRAINZ_ALBUMID` |
| MusicBrainz Release Group Id | `MUSICBRAINZ_RELEASEGROUPID` |
| MusicBrainz Work Id | `MUSICBRAINZ_WORKID` |
| MusicBrainz Disc Id | `MUSICBRAINZ_DISCID` |

Arbitrary additions retain their entered separators and uppercase ASCII
letters. Invalid Xiph keys, invalid UTF-8, exact empty values, and artwork keys
are blocked rather than sanitized or reinterpreted. The mapping table is exact
apart from Vorbis-comment ASCII case: `ALBUMARTIST` is semantic
`albumartist`, while `ALBUM ARTIST` is a separate freeform property. A logical
edit removes/replaces only explicitly mapped native properties for that
semantic field. A freeform edit addresses only its exact native key.

## Reading and merge precedence

A playable item may receive metadata from the file/container, an external cue,
a sidecar, Trackbench's persisted annotations, a stream, and a playlist
snapshot. Preserve provenance rather than flattening immediately.

**Trackbench decision (ADR-0033):** effective-value precedence is:

1. explicit user sidecar override;
2. metadata belonging to the logical segment/cue track;
3. current embedded/container metadata;
4. current Trackbench-only annotation/statistic for its namespace;
5. cached playlist snapshot when the source cannot be read.

Per ADR-0066, FFmpeg's generic `track`, `disc`, and `album_artist` probe keys
are explicit stream projections of semantic fields, not proof of native tag
identity. When TagLib already exposes the corresponding embedded semantic
property, the secondary projection is omitted; the same rule removes affected
persisted stream duplicates on restore. A genuinely embedded freeform `TRACK`,
`DISC`, or `ALBUM_ARTIST` property remains independently visible and mutable.

ReplayGain has its own rules in `replaygain.md`. A user must be able to inspect
the source of an effective value and choose the write target.

## Editing covers

In **Edit tags… → Artwork**, select individual covers with Ctrl/Shift or all
rows with Ctrl+A. **Remove** (or Delete) stages removal of the selected embedded
pictures, including multiple versions of the same type in one file. External
image files remain on disk; selecting them does not block embedded removal.

Use **Add image…** for your PNG/JPEG or **Fetch cover** / **Covers…** for archive
images. Review the pending changes, then use the main **Apply** button to save
covers and any enabled tag edits together. Nothing is written before Apply;
The pending list shows **Before** and **After** thumbnails, with explicit
**None** and **Removed** labels for additions and removals. Incoming previews
load in the background; unavailable previews do not bypass Apply validation.
**Undo selected** removes selected entries from the pending list, and
**Discard changes** clears the artwork draft. Fetching a front replaces every
existing front variant with one front, retaining other cover types. Apply or
discard before changing the selected files.

Unchecking **Save tags** leaves text edits unwritten while still applying staged
covers. Each physical file is prepared and verified once before publication,
with one recovery backup for all its changes. A failed replacement leaves that
file's original tags and pictures intact; one operation undo restores both.
Cancellation or failure can still leave earlier files committed in a multi-file
batch. Feedback reports those outcomes. For physical tag/artwork saves, use
**Retry failed / stopped files** to retry only unfinished files. Previously saved
files are skipped. Changed source files and unresolved recovery records remain
blocked; reopen changed files to review them again. This retry control does not
yet cover rename/move, CUE sheets, or loudness sidecars (ADR-0161).
External image deletion and artwork
mutation in unqualified containers remain outside this workflow (ADR-0160).

## Tag editor

**Edit tags…** in the Edit menu and local-track context menu opens this
workspace; **Alt+Return** is its keyboard shortcut. The workspace tab is named
**Tags · N tracks**. Earlier specifications call this workspace Properties.

Opening one or many locally resolved tracks produces a non-modal, job-backed
workspace optimized for keyboard and bulk data entry. It must not resemble a
form where every destination field is clicked individually or selected from an
endless fixed dropdown.

The primary surface is a vertical split with a virtualized file selector above
a field-oriented table with Field, Original, and Draft columns. The file selector
shows each full source path, with the unabridged path also available in its
tooltip when the column is too narrow. Selecting one
file gives an individual view; selecting several files gives a bulk view. It
provides:

- editable field columns/rows and separately visible read-only technical data;
- a state for common value, mixed values, missing everywhere, and partially
  present;
- individual per-track values in selection order;
- arbitrary new fields;
- type-to-add field creation with fuzzy completion over present, conventional,
  MusicBrainz, and recently used names;
- direct keyboard traversal, range selection, fill, clear, copy, and undo;
- field-name and changed-only display filters plus saved cleanup scripts;
- correct multi-value editing without semicolon-as-data ambiguity;
- field removal and complete tag removal as distinct operations;
- artwork and ReplayGain sections;
- reload/discard and external-change conflict handling;
- a complete preview before writing.

**Trackbench decision (ADR-0170):** the Fields toolbar saves bounded named
layouts from selected or currently visible fields. A chosen layout filters the
presentation immediately and supplies preferred field order when the next tag
selection opens. The active layout and its ordered canonical field names use a
versioned workspace record. Layouts never remove fields from the underlying
selection: Apply and undo still include hidden drafts.

**Trackbench decision (ADR-0034, M5 read-only grid baseline):** Properties is
now a non-modal workspace reachable from Edit, the track context menu, and
`Alt+Return`. It preserves every selected occurrence as a row and shows a
preferred-first sparse union of ordinary, arbitrary, and MusicBrainz fields as
columns. Headers report common, mixed, missing, or partial state from exact
ordered value vectors; selecting a cell reveals its unjoined values and winning
provenance. Equal values with different provenance remain common. Projection is
bounded, snapshot capture yields between short event-loop slices, aggregation
is dispatched after the dialog shell opens, and the Qt model creates no
persistent per-cell objects. That baseline was deliberately read-only and had
no edit trigger or Apply action.

**Trackbench decision (ADR-0035, M5 draft baseline):** Properties now layers a
bounded sparse in-memory draft over that immutable selection. Keyboard or
double-click editing stages one exact scalar value; repeated values are shown
with an explicit replacement prompt and are never parsed from joined display
text. Delete stages field removal, which remains distinct from replacement;
undo/redo, selected-cell revert, whole-draft discard, restrained draft styling,
and exact Original/Draft inspector rows expose every change. ADR-0041 now
projects complete Draft result states in the background. Closing a dirty
workspace requires discard.
There is still no Apply or file-write path.

**Trackbench decision (ADR-0036, M5 aggregate baseline):** Properties now opens
on the compact Fields/Original/Draft projection. Common Original values remain
exact, while mixed, partial, and missing states stay explicit. Editing Draft
stages one value across the complete selection; Delete removes the field and
`Ctrl+Backspace` reverts it. The Tracks page retains occurrence-aligned cells
and the exact values inspector. Both projections share the same bounded patch
set and undo history. Known uniform bulk intent is shown directly; ADR-0041
projects exact aggregate results after individual exceptions.

**Trackbench decision (ADR-0037, M5 ordered-value baseline):** `Edit values…`
and `Ctrl+Enter` open a non-blocking structured editor for the current bulk
field or individual track cell. One row is one exact value; add/remove/reorder
preserve order, duplicates, delimiter characters, and explicit empty strings.
Zero rows cannot be accepted because field removal remains the separate Delete
operation. Accepted vectors enter the same bounded patch and undo history as
scalar edits, with no file-write path.

**Trackbench decision (ADR-0038, M5 selection-driven Properties):** the separate
Fields and Tracks modes are superseded by one vertical split. A read-only,
multi-select file list sits above Field/Original/Draft, and its selected rows
are the only individual/bulk edit scope. One row exposes exact Original and
Draft values; multiple rows expose common, mixed, partial, or missing states.
Nontrivial subset summaries are debounced, generation-safe, and computed off
the UI thread. Scalar edits, exact ordered values, removal, revert, undo/redo,
and discard continue to share one bounded sparse draft; there is still no
Apply or write path.

**Trackbench decision (ADR-0039, M5 dynamic field vocabulary):** `Add field…`
and Insert accept an arbitrary field name, append a missing row to the current
session vocabulary, and focus its Draft cell. Canonical duplicates select the
existing row. The operation shares immutable per-occurrence baselines instead
of copying the complete selection, and it preserves the selected-file scope
while the backing model gains its hidden field column. `Remove field` and
Delete stage explicit removal across the selected files; for a newly added
field, removal cancels its staged additions. Removed rows remain visible for
partial scopes, revert, and undo.

**Trackbench decision (ADR-0040, M5 field-name completion):** `Add field…`
offers at most 12 deterministically ranked names from fields present in the
selection, workspace-recent additions, a conventional catalog, and the initial
MusicBrainz identity/sort-name set. Matching ignores ordinary field separators
and ranks exact, prefix, substring, then ordered-subsequence matches. Canonical
duplicates prefer the spelling already visible in the selection. The popup is
only a shortcut: arbitrary names remain valid, and catalog membership implies
no write capability.

**Trackbench decision (ADR-0041, M5 complete Draft projection):** the Draft
column is a complete projection of the current selected files plus their sparse
patches. A 40 ms debounce dispatches one cancellable worker over an immutable
copy-on-write snapshot; generations reject obsolete scope/edit results. Sparse
traversal yields exact common values and common/mixed/partial/missing states
without materializing the item-by-field matrix. Pending non-uniform rows remain
labelled as preparing. This is still an in-memory result preview, not a
revalidated file-write plan or Apply capability.

**Trackbench decision (ADR-0042, M5 revalidated write-plan preview):**
`Preview write plan…` snapshots the complete draft and rereads every distinct
staged raw source once on a cancellable worker. The Qt-free plan compares
captured and observed revisions, retains every exact logical occurrence intent,
merges compatible fields for shared paths, and blocks missing/changed sources,
incompatible CUE/duplicate results, non-embedded targets, distinct paths that
alias one inode, unavailable writers, and unproven unknown-data preservation.
Its virtualized table shows Fresh original beside Planned result for every
intent. ADR-0047 now adds Apply only when the complete immutable plan is ready;
blocked previews remain inspection-only.

**Trackbench decision (ADRs 0043–0047, M5 native-FLAC transaction baseline):**
one compatible physical source can be materialized as an exclusive
preservation-verified sibling and committed by the Qt-free operations layer.
The executor locks and revalidates the direct single-link file, journals the
complete immutable plan and raw sibling paths through reversible SQLite
migration 6, preserves owner/mode/bounded Linux extended attributes, creates an
exact hard-link backup, atomically renames the prepared file over the source,
and rereads its revision, document, and every planned field. Completion also
requires an idempotent all-or-nothing callback to refresh every affected
logical occurrence. Failure restores the recorded original identity; startup
recovery completes only unambiguous publication and otherwise preserves
evidence as `needs_reconciliation`. Migration 7 implements that callback as a
provenance-aware transaction over every raw-path occurrence plus a durable
source cache: embedded/stream layers advance while annotations and logical
CUE/chapter/subsong/sidecar overlays remain intact, and recovery replay is
idempotent. Legacy flattened logical snapshots require a fresh probe instead of
guessing provenance. Migration 8 keeps the backup lifecycle separate from
publication state. Trackbench runs recovery and bounded maintenance off the UI
thread after list/cache initialization, opens Metadata operations automatically
for ambiguous evidence, and offers verified single-step undo or explicit backup
release for retained records. Undo atomically exchanges the recorded old and
published identities, rereads and verifies the original fields, and refreshes
all occurrences under a fresh idempotency identity before deleting the replaced
publication. Interrupted undo is recoverable. The fixed initial retention policy
runs at startup and keeps backups for up to seven days, only the newest per exact
raw source, and at most 256 entries/10 GiB globally; ambiguous evidence is never
deleted for a budget. Apply captures and persists the current workspace before
admitting file mutations, then feeds only a wholly ready plan through two
bounded workers. Cancellation stops new admission while in-flight sources reach
a safe boundary. The result keeps plan order and reports committed, failed, and
cancelled sources independently; committed sources refresh every durable and
visible occurrence plus operation history. Because a batch is not a fictional
cross-filesystem transaction, successful sources remain committed after an
unrelated failure, and every retry requires a fresh complete preview.

ReplayGain records appear in the same inspection workspace because users
reason about them as metadata. Analysis remains a separate PCM job with its own
algorithm/provenance and cannot be faked as an ordinary text-field edit.

Server items live in Trackbench's MPD Queue authority, which displays their
metadata but offers no file-write actions. Trackbench edits only sources
explicitly opened under its Local Queue authority; opening a mapped server item
as a local source is a deferred cross-authority convenience (ADR-0058), not an
eligibility rule for mutation.

Edits are staged in memory. A staged document must record the source revision
(file identity, size, mtime, and preferably tag hash). If a file changes before
commit, stop or request reconciliation rather than overwriting newer metadata.

### Metadata providers

A provider may inspect current metadata, path context, durations, fingerprints,
or other explicitly supplied evidence and return proposed fields, identifiers,
artwork references, provenance, and confidence. It cannot mutate a source. Its
proposal enters the ordinary properties/tag workspace, where alignment,
ambiguity, source revision, and every resulting value remain previewable before
commit.

An online MusicBrainz provider is the committed first implementation (M6) and
must be an explicit network action. Build and validate the Trackbench-owned
provider boundary with real metadata workflows before freezing a public plugin
ABI; other lookup providers should reuse the same proposal contract rather
than receiving direct tag-writer access.

### Auto track numbering

Assign track numbers by visible/selection order, with configurable grouping.
foobar2000 behavior groups different folders as separate albums and writes
`TRACKNUMBER` plus `TOTALTRACKS`. Trackbench should show the grouping and
result before commit and allow grouping by a title-format expression.

### Fill values from filenames or text

The inverse of formatting parses a source string into captures:

```text
source:  Artist/Album [2024]/03. Title.flac
pattern: %artist%/%album% [%date%]/%tracknumber%. %title%
```

foobar2000's “Automatically Fill Values” capture pattern is **not** title
formatting: functions and optional-section brackets do not apply. `%%` ignores
a captured portion, and filename mode can traverse parent directories when the
pattern includes separators.

**Trackbench decision (ADR-0068):** `tkcapture-1` is a distinct versioned,
whole-input grammar. `%field%` captures an exact value, repeated fields append
ordered values, `%%` captures and discards a portion, and a backslash quotes the
next Unicode scalar. Captures may be empty so present-empty remains distinct
from missing. The bounded matcher returns unmatched, unique, or ambiguous and
never chooses a greedy parse. Sources include:

- full path or filename;
- a title-format expression evaluated per item;
- existing field(s).

Filename mode removes the final extension and includes one parent component per
literal `/` in the pattern; full-path mode retains both. Field mode applies the
pattern to every ordered value, and formatted mode evaluates `tkfmt-1` against
the current chain state. One action can therefore replace several target cells,
all visible in the ordinary immutable preview. Adapter-mapped names become
semantic fields; every other spelling remains a visibly exact native field, so
capture cannot recreate separator-derived aliases. Ambiguous or unmatched
input, missing sources, invalid UTF-8, and limits block the complete preview.
Reversible migration 20 persists action code 16 with exact capture dialect and
source kind.

## Saved bulk transformation chains

The foobar2000 Masstagger model—an ordered list where later actions see earlier
results—is worth preserving, with a modern preview and undo journal.

Required actions:

- set a field to a literal value;
- add a value without destroying existing values;
- remove a field;
- remove matching values;
- replace matching values;
- copy one field to another;
- format a field from a pure `tkfmt-1` expression;
- split a field by an exact separator;
- join/merge values explicitly;
- auto-number tracks;
- guess/capture values from a filename/path or other formatted source;
- remove every field except an allowlist;
- remove named unwanted fields with a blocklist;
- embed or remove a cue sheet where supported;
- manipulate artwork through explicit actions.

Chains are named, reorderable, importable/exportable, and versioned. Preview
shows the original and final document plus optionally each intermediate step.
Do not serialize executable host-language code; persist declarative action data
and formatting source.

**Trackknife decision (ADR-0178):** the structured script editor implements
field allowlists and blocklists as typed, reusable actions. Names are entered as
a comma-separated list and matched through canonical logical field identity.
The ordinary transformation preview lists every removal before **Add to draft**;
Apply remains the only file-writing step. Database schema 34 and native JSON
preserve these actions exactly. They are deliberately unavailable in the
smaller raw cleanup-script syntax rather than being translated ambiguously.

**Trackbench decision (ADRs 0048–0053 and 0064–0066, M5 transformation
slices):** schema 1 chains are Qt-free ordered declarative data evaluated
against the selected items' current staged draft. The first actions set exact
values, remove a field, trim/lowercase/uppercase each existing value without
flattening the list, capitalize only the first Unicode character of each
existing value while leaving its remainder unchanged, or set one scalar from
the typed metadata-transformation `tkfmt-1` host.
Exact add appends values in order without deduplication; copy mirrors the full
source field state and removes the target when the source is missing; split
uses one non-empty exact separator while retaining leading, trailing, and
adjacent empty components; join writes one exact value and permits an empty
separator.
Later actions observe all earlier results. An empty format result is one
explicit empty value, not removal. A cancellable bounded planner emits only
changed final cells with exact missing/value states and the last writer step.
Properties requires that immutable preview and stages it only if every before
value still matches the live draft; all cells then enter the existing sparse
patch model as one undoable transaction. The normal fresh physical write plan,
format capability checks, and Apply remain mandatory. Reversible SQLite
migrations 9–11 store at most 256 non-empty, exact-name chains in normalized
ordered action/value rows with stable schema codes. Properties loads, saves,
updates, saves-as-new, and deletes them through the serialized persistence
worker. Its **Tagging scripts** side panel lists every definition with a
persistent checkbox and opens the selected row in the editor. Checked chains
run in displayed name/stable-ID order after manual draft edits and before fresh
write-plan revalidation. Their results are applied to a temporary draft copy,
so the final immutable plan contains the complete tagging operation while the
visible draft and undo history remain manual and repeated previews cannot
compound additive actions. Explicit one-off transformation preview/staging and
the final explicit Apply boundary remain available.
Per ADR-0052, Properties is a temporary protected `Tags · N tracks` workspace
tab rather than a separately sized top-level window. Its transformation preview
shows Field/Old/New at the top level and retains affected-file plus producing-
step diagnostics as an expandable child beneath each exact changed cell.
Transformation target fields reuse ranked completion over names actually
present in the selected tags. Separator-insensitive ranking is discovery only:
distinct semantic and freeform results retain distinct mutation addresses.
Arbitrary new target names remain valid.
Per ADR-0053, remove-matching and replace-matching compare complete values by
case-sensitive valid-UTF-8 byte equality, with no normalization, substring,
glob, regular-expression, or locale behavior. Remove deletes every match and
makes the field missing when none remain; replace expands every match to a
bounded non-empty ordered replacement sequence, where an empty replacement
string remains a present value. Selection-order numbering replaces the target
with one consecutive decimal value in captured file order using a bounded
start and optional minimum-width zero padding. It neither infers groups nor
writes `TOTALTRACKS`; grouped/title-format numbering must later preview its
boundaries and totals explicitly. Reversible migration 12 stores these three
typed actions under stable codes without reinterpreting older definitions.
Per ADR-0064, **Keep first characters of each value** exposes exact prefix
extraction without requiring a formatting expression. Its bounded positive
count is measured in Unicode scalar values and defaults to 4 in the editor;
each value is transformed independently, short and empty values remain exact,
and a missing target stays missing. It is not a date parser, but `DATE` with a
count of 4 turns `2024-08-30` into `2024`. Reversible migration 17 stores this
typed action under stable code 14 without reinterpreting older definitions.
Per ADR-0065, **Paste script…** translates a bounded Picard-style cleanup
subset into the same editable typed list; it neither stores nor executes the
foreign source. `$unset`/`$delete`, `$set`, and `$if` combine with a small pure
field/conditional/`$left` subset. Unsupported calls fail closed with
source-positioned diagnostics. `$unset` explicitly means actual Trackbench
field removal, not Picard's separate old/new-metadata behavior. Conditional
deletions use a typed `tkfmt-1` predicate evaluated at that chain position;
migration 18 stores it under stable action code 15. Matching `$set` targets
across true/false branches collapse into one conditional value rule, while a
self-prefix cleanup guarded by the same field remains a no-op when absent and
becomes a normal keep-first rule. Picard's `comment:` default-comment target
maps visibly to Trackbench's conventional `COMMENT`; wildcard deletion remains
unsupported. Per ADR-0066, translated `$unset` and `$delete` actions address
the exact adapter-exposed native name, with format-defined case handling and no
separator aliasing. A hand-authored **Remove field** action remains semantic.
Migration 19 persists exact-native removal and carries the same address through
the operation journal. ADR-0068 adds the separate `tkcapture-1` multi-target
action and schema 20 saved-chain representation. Per ADR-0070, the editor also
projects representable typed cleanup actions into a deterministic **Raw
script** tab. Valid raw edits immediately regenerate the typed action list;
invalid text blocks Preview and Save, while typed-only actions make Raw mode
read-only with an exact step diagnostic. The typed actions remain the saved
authority, so pasted whitespace and spelling are canonicalized after reload.
Dirty name, typed, pasted, and raw edits require Save or explicit discard.
ADR-0072 adds the separate strict native JSON interchange form for the complete
typed chain, including exact ordered values and dialect-qualified formatting
and capture actions. It deliberately omits saved identity and automatic state;
an import is an unsaved definition that still needs review, preview, and Save.
Grouped numbering and richer match dialects remain future slices.

## Artwork

Artwork is typed (`front`, `back`, `artist`, `disc`, `icon`, `other`) and carries
MIME type, description, dimensions, byte size, and provenance. Support:

- embedded pictures using the format's native representation;
- external search patterns relative to the track/album path;
- configurable source order and fallback names;
- view, add, replace, remove, export, and copy across selected tracks;
- deduplication by content hash;
- warnings for very large images or formats rejected by a container;
- preserving unrelated embedded images during tag writes.

For Vorbis-comment containers, `METADATA_BLOCK_PICTURE` is the preferred
interoperable embedded representation. FLAC uses native picture blocks; ID3 uses
APIC; MP4 uses `covr`. Exact mappings belong to adapter specs/tests.

**Trackbench decision (ADR-0076):** the first read-only artwork boundary
inventories every native FLAC picture block and only exact caller-configured
sibling basenames. Default external fallback order covers `cover`, `folder`,
`Folder`, and `front` JPEG/PNG names without globbing, recursion, case folding,
or UTF-8 conversion of raw path bytes. Each item retains its exact native type,
smaller Trackbench role, MIME, description, optional dimensions, encoded size,
SHA-256 identity, embedded/external provenance, raw source path and revision,
and ordinal. Equal bytes retain both records with explicit duplicate linkage.
Missing external names are ordinary absence; present malformed, unreadable,
changing, or oversized files produce typed issues. Encoded bytes are inspected
transiently and never persisted. This read boundary alone qualifies no picture
write; export, copy, and every non-FLAC embedded mapping remain unqualified.

**Trackbench decision (ADR-0077):** Properties places **Fields** and
**Artwork** beneath the same file selector; the selected rows remain the sole
scope for either section. Artwork inventory begins only while its section is
active, collapses repeated logical occurrences by exact raw media path, and
runs sequentially on one cancellable worker. A scope above 64 physical sources
is rejected visibly rather than partially inventoried. Source rows expose
embedded and exact-sibling read capability, then-unavailable change capability, and
captured-versus-observed revision state. Inventory and issue rows expose the
complete typed ADR-0076 evidence without decoding pixels. The presentation is
session-only: no inventory row or encoded image is stored in SQLite. ADR-0080
supersedes the read-only control state for qualified native FLAC sources.

**Trackbench decision (ADR-0078):** the first immutable artwork write plan
addresses one exact native-FLAC embedded picture by captured media revision,
native ordinal, and original SHA-256. Equal logical intents collapse by raw
media path; revision disagreements, conflicting intents, physical aliases,
missing/changed targets, and stale or unsupported replacement input block the
source visibly. Replace rereads one exact PNG/JPEG path and retains only its
revision, MIME, dimensions, size, and SHA-256 evidence in the plan. It changes
the selected picture's bytes/MIME/dimensions while retaining native type and
description; remove deletes only that ordinal. The prepared-copy adapter proves
the final inventory, exact serialization/order of unrelated pictures, unchanged
text and non-picture/non-padding blocks, and byte-identical compressed audio.
No encoded image is retained or stored in SQLite. ADR-0079 supplies the journal
extension and recovery verifier, and ADR-0080 now publishes this plan from
Properties.

**Trackbench decision (ADR-0079):** reversible migration 23 gives the existing
metadata-operation journal a text-versus-embedded-artwork content kind and one
compact artwork evidence row. It records the reviewed ordinal, item counts,
target/replacement SHA-256 identities, and versioned SHA-256 digests of the
complete original and planned ordered embedded inventories. It records no
image payload, pixel data, replacement file, or inventory row. Artwork and text
share the same locked prepared-sibling, retained-old-inode, atomic replacement,
dependent-state, rollback, undo, and restart-recovery lifecycle. Recovery
rereads the published source and proves its revision and complete planned
inventory before replaying the idempotent all-occurrence metadata refresh; undo
proves the complete original inventory. ADR-0080 now qualifies bounded review
and Apply.

**Trackbench decision (ADR-0080):** Properties enables **Replace…** and
**Remove** only for selected embedded rows from revision-matching native-FLAC
sources with the durable mutation service available. The shared file selector
expands each selected target to every matching logical occurrence; external
sibling images remain read-only. Each action builds a new cancellable,
immutable fresh-file review with per-source operation, target role/ordinal,
replacement path, affected tracks, and blockers. Apply exists only for a
wholly ready plan. A two-worker Qt-free job reports ordered per-source progress
and partial results, cancellation stops new admission, and retry always begins
with another fresh review. Successful sources use the schema-23 journal,
refresh every durable and visible occurrence, advance the section revision,
and trigger a new file-backed inventory without closing Properties. The
prepared-copy adapter now rewrites only the selected native FLAC picture block
and streams every other block plus compressed audio byte-for-byte; TagLib does
not save the whole file and cannot normalize unrelated metadata.

**Trackbench decision (ADR-0081):** **Add…** applies one selected PNG/JPEG and
canonical role to every revision-matching native-FLAC source in the Properties
file scope. Fresh validation derives an append ordinal and rejects duplicate
encoded SHA-256 content. Schema 24 represents Add with a null original target,
replacement hash, original/planned counts, and complete inventory digests;
publication, restart recovery, refresh, and exact Undo remain the ADR-0079
lifecycle. **Copy to Selection** is Add with one inventoried donor: embedded
donors are reread transiently by exact revision/ordinal/hash without a temp
file, retain role/description, and exclude their own media source. External
siblings can be donors but are never modified. **Export…** rereads selected
embedded or external rows in a two-worker cancellable job and exclusively
creates deterministic `artwork-N-role` outputs with a MIME-derived suffix; it
never overwrites, journals, or retains backups. Other container writers remain
unavailable.

## Filesystem rename, copy, and move

**Trackbench decision (ADRs 0054–0055 and 0069):** rename/move belongs to the
same typed preparation plan as optional tag persistence and qualified ReplayGain storage.
The tagging workspace will expose independent Save tags, Rename files, Move
files, and ReplayGain choices only as each operation gains a real immutable
preview and journaled Apply path. When Save tags is on, checked scripts and
manual edits produce the final in-memory metadata context before path
evaluation. When Save tags is off, Rename/Move instead uses only the captured,
revision-qualified source tags: manual drafts remain visible but excluded, and
automatic chains are not evaluated for the path plan.

A versioned output-layout profile stores separate relative-directory and
basename `tkfmt-1` expressions plus its sanitization-policy version. An absolute
raw-path destination root is a separate named profile. Rename without Move
changes only the basename in the current directory; Move without Rename applies
the directory layout below the selected root while preserving the original
basename; enabling both evaluates both components. Existing-file operations
preserve the source extension. The converter may reuse both profile types while
owning its output extension and permitting explicit per-job overrides.

Combined content/path operations prepare and verify the final file directly at
the destination rather than rewriting the source and then moving it. Same-
filesystem path-only changes may use atomic rename; cross-filesystem moves must
copy, verify, durably publish, and only then delete the source. The recoverable
journal and dependent-state transaction cover raw source/target paths,
sidecars, list occurrences, playback state, metadata cache, and statistics.

The operation accepts a selected track set, destination root, and `tkfmt-1`
relative-path pattern. Separators in the evaluated pattern request
subdirectories below the explicit root.

The first implemented `linux-v1` policy treats those directory separators as
structure, replaces `/` in a generated basename and all generated NUL bytes
with `_`, maps empty and `.` components to `_` and `..` to `__`, and performs
no Unicode normalization, transliteration, whitespace trimming, or portable-
platform reserved-name rewriting. Raw and sanitized results remain side by
side in the immutable plan. The pure planner consumes captured source
revisions plus an explicit existing-path snapshot, detects device/inode aliases
and lexical conflicts, and performs no I/O. Symlink/mount resolution,
permissions, exact filesystem limits, and publication capability require a
fresh later preflight and are not implied by a ready pure plan.

**Trackbench decision (ADR-0169):** saved layouts may instead select
`portable-v1`. It additionally replaces ASCII controls and Windows-forbidden
punctuation, trailing dots/spaces, and reserved device stems. It deliberately
does not normalize or transliterate Unicode. The choice is stored with the
layout and shown in the live preview; persisted `linux-v1` behavior is
unchanged.

**Trackbench decision (ADR-0056):** the fresh preflight now walks every existing
path component from `/` without following symlinks, requires an unchanged
single-link regular source, rejects newly occupied targets and unusable
parents, tightens component/path limits from the target filesystem, and lists
missing target directories without creating them. Source/target device
identity explicitly classifies atomic rename versus cross-filesystem copy.
Migration 14 persists fixed prepared-target, published-target, dependent-state,
source-removed, complete, rollback, and reconciliation boundaries. This is
recovery structure rather than mutation authority.

**Trackbench decision (ADR-0057):** the first executor slice now qualifies
same-filesystem path-only publication. It repeats descriptor-relative source,
target, device, access, and missing-directory checks; journals before `mkdirat`;
publishes only with `renameat2(RENAME_NOREPLACE)`; syncs both directory entries;
and verifies the original inode at the target. An idempotent all-or-nothing
dependent-state callback defines the rollback boundary: failure before callback
success restores the exact source path, while a journal failure afterwards
retains the target and replays the callback during startup recovery. Ambiguous
topology is never deleted. Case-only aliases on case-folding filesystems remain
unsupported because ordinary rename cannot retain the no-replace guarantee.
ADR-0057 itself does not define the concrete dependent-state schema, undo,
cross-filesystem copy, bounded batch execution, or UI choices.

**Trackbench decision (ADR-0059):** migration 15 and the serialized list
repository now supply the concrete all-occurrence path callback. Every exact
local source occurrence must carry the captured previous revision; the one
transaction advances all duplicate/logical rows to the target and published
revision, re-keys any verified metadata cache, rejects target-state collisions,
and records idempotent operation evidence. Ordered relocation records are
replayed over load and replace-all snapshots only when path bytes and revision
both match, so delayed A snapshots converge through A→B→C while a different
file reusing A remains A. The visible list model follows the same guarded update
and retains its current-row anchor. Later ADRs add same-filesystem undo,
cross-filesystem verified copy, active-playback reconciliation, batching, and
workspace choices over this callback.

**Trackbench decision (ADR-0060):** a same-filesystem undo is a second durable
publication record linked to its completed forward operation. It locks and
revalidates the exact published target, requires the original parent and an
absent original name, journals before a no-replace reverse rename, syncs and
verifies both entries, then sends a new B→A identity through the same ADR-0059
all-occurrence callback. Dependent failure restores B; a crash after callback
success leaves A and replays the idempotent reverse record at startup. Rolled-
back attempts may retry, while a completed reversal makes repeated undo a
verified no-op. Directories created for the forward target are never removed.
ADR-0067 exposes this qualified undo through preparation-operation history.

**Trackbench decision (ADR-0061):** cross-filesystem path-only Move now copies
through the exact journal-derived target sibling with a bounded buffer,
preserves and verifies ownership, permissions, timestamps, and bounded Linux
extended attributes, syncs the candidate, and compares every byte with the
still-locked source. The sibling is published only through a no-replace rename;
the ADR-0059 all-occurrence transaction then advances durable references before
the exact original is unlinked and its parent synced. Startup recovery can
adopt an exact copy that preceded its transition, infer publication or source
removal on either side of their journal boundaries, and replay dependent state.
Unexpected identities or content remain visible for reconciliation. Cross-
filesystem undo remains unavailable; ADR-0063 adds batching and ADR-0067 adds
workspace controls.

**Trackbench decision (ADR-0062):** local audition captures exact revisions for
the current decoder and its prepared gapless continuation. The file-publication
callback serializes an exact source-to-target re-key on the audio worker without
reopening the decoder, flushing PCM, seeking, or reconnecting PipeWire. Already
queued load/continuation intents follow the target and require its published
revision; exact recovery replay is a no-op, while a reused path is left alone
and reported. Audio advances before the durable ADR-0059 list/cache transaction
and is inversely compensated if that transaction fails, allowing the executor's
filesystem rollback to restore one coherent source state. Cross-filesystem
undo remains unavailable; ADR-0067 adds workspace controls.

**Trackbench decision (ADR-0063):** one entirely ready filesystem review now
enters a bounded 1–8-worker Apply job and returns ordered results for every
physical source, including explicit no-change, failure, and cancellation
states. Each admitted source receives a fresh single-source preflight before
same/cross-filesystem dispatch. Sources sharing a reviewed missing-directory
root serialize until executor-proven in-batch creation establishes each
required path; unrelated and already-established targets may publish
concurrently. ADR-0075 retains that exact directory evidence if later work for
the creating source rolls back, without trusting an externally appeared path.
Progress delivery and completed counts are serialized, cancellation stops new
admission, and in-flight executors reach their journaled safe boundary.
ADR-0067 adds workspace controls; cross-filesystem undo remains unavailable.

**Trackbench decision (ADRs 0067, 0069, and 0074):** Properties retains tag effects,
pure paths, and live filesystem preflight in one immutable preparation review.
It materializes the final manual-plus-automatic metadata draft only when Save
tags participates. Rename and Move
are enabled only with their saved profile dependencies and the real
file-publication service. With Save tags off, path expressions see the actual
source-tag snapshot and cannot receive a manual or automatic synthetic value;
the core rejects such a malformed plan. Path-only Apply first persists the
captured workspace, then uses the bounded ADR-0063 job and
the composed active-player plus all-occurrence dependent callback; progress,
partial results, cancellation, visible-source refresh, and fresh-preview retry
remain explicit. ADR-0074 removes the earlier changed-tags blocker for a fully
ready native-FLAC text plan: exact path/revision pairing selects the composed
artifact executor and one durable metadata-plus-relocation callback. Startup
recovers both filesystem state machines, presents their records beside metadata
operations, and offers linked same-filesystem undo only for byte-preserving
publication when no non-rolled-back reversal exists. Cross-filesystem moves and
changed artifacts remain visible but truthfully non-undoable.

**Trackbench decision (ADR-0073):** file-publication topology and content intent
are orthogonal durable evidence. Reversible migration 21 distinguishes an
exact byte-preserving path operation from a verified changed destination
artifact. The latter always prepares an executor-owned sibling in the target
directory, even on the source filesystem; publishes without replacement;
commits dependent state while the exact original still exists; and only then
removes that locked original. Recovery uses exact byte comparison for ordinary
copies, but uses the durably recorded artifact revision after `target_prepared`
because changed metadata cannot equal the source bytes. A changed artifact
found before that revision transition is retained for reconciliation rather
than adopted or deleted. The real native-FLAC writer proves this single-source
path directly at a changed destination. ADR-0074 composes that core into
bounded Apply and startup recovery; path-only behavior is unchanged.

**Trackbench decision (ADR-0074):** one ready preparation batch may contain
metadata-only, path-only, combined metadata/path, and no-change physical
sources. Metadata and path work pair only on an exact raw source path and
captured revision. Combined native-FLAC work prepares and verifies the changed
artifact directly beside its reviewed destination; the active-player barrier
then composes with one schema-22 transaction that relocates every occurrence,
preserves logical overlays, installs the published embedded/stream metadata and
target revision, and records idempotent refresh intent. Startup recovery rereads
the exact published artifact and uses the same transaction rather than storing
a duplicate metadata document in the file journal. Visible rows receive both
the target path and published metadata. Changed artifacts have no current undo
surface.

**Trackbench decision (ADR-0083, M5 direct apply):** the immutable preparation
plan is no longer presented in a routine review dialog, and Apply progress no
longer opens a modal window. **Apply** plans, preflights, and — when every
enabled effect is ready — immediately runs the same bounded Apply job, with a
progress bar, `n of N` status, and **Stop** in the Properties footer. Blocked
plans change nothing and open one compact feedback window listing only the
offending files; stopped and failed runs report untouched files the same way,
while fully committed runs close Properties silently. Trust in file naming
moves to the resizable naming-layout manager, whose live bounded preview table
shows each selected track's resulting path as the expressions change. The
revalidation, preflight, journal, recovery, and history contracts of ADRs
0042–0082 are unchanged; ADR-0085 later extends the same contract to artwork.

**Trackbench decision (ADR-0084, M5 silent recovery):** the Preparation
operations history/undo window is removed. Journaled crash recovery still runs
at every startup, but silently; only operations recovery could neither finish
nor safely roll back appear — exactly once each — in the compact ADR-0083
feedback window, with acknowledged incidents remembered in settings. Undo
backups are still created inside the atomic commit protocol but are released
at startup, since no undo surface consumes them; same-filesystem move undo is
gone with its only entry point. In exchange, the tag grid itself now carries
Picard-style change semantics on the changed content only: added fields in
green text, changed values in orange, removed fields in struck-through red,
across both the per-file grid and the Field/Original/Draft rows, with no
background painting.

**Trackbench decision (ADR-0085, M5 artwork thumbnails and direct apply):**
the Artwork tab presents pictures, not plumbing. Every inventory row leads
with a thumbnail decoded in the existing background job from transient
revision-qualified rereads; columns compress to File/Role/Image/Source with
fingerprints, native types, and ordinals in tooltips. The storage note and
the permanent capabilities table are gone — only genuinely view-only files
appear in the problems pane, with one plain reason. Add/Replace/Remove/Copy
revalidate as before but a ready plan enters the bounded batch executor
immediately; progress and Stop are inline beside the status, Export drops
its modal progress dialog for the same pattern, and blocked, stopped, or
failed work opens the compact ADR-0083 feedback window. The inventory core,
write plans, journaled commits, and recovery are unchanged.

**Trackbench decision (ADR-0086, M5 provider boundary):** metadata providers
are Qt-free observation-only functions over the draft-effective selection
that return typed proposals: ordered replacement field values (identifiers
travel as ordinary fields), artwork references, provider provenance, and
per-proposal confidence with a human rationale. One validated conversion
drops low-confidence and already-satisfied proposals and stages the rest as
a single ordinary draft transaction — colored in the grid, one-step
undoable, and revalidated by the same direct Apply as hand-typed edits.
Providers never mutate or write. The internal selection-consistency
provider (**Suggest**) proves the boundary before any network: it fills a
missing ALBUMARTIST only on exact in-group agreement and proposes
TOTALTRACKS only for an exactly contiguous 1..N run per album group. M6
MusicBrainz implements the same contract; candidate ranking and comparison
surfaces are its UI work.

**Trackbench decision (ADR-0087, M5 paired totals identities):** the
TRACKTOTAL/TOTALTRACKS and DISCTOTAL/TOTALDISCS spellings are one
explicitly enumerated Picard-style identity, not an inferred alias.
Canonicalization merges them, so every canonical-keyed surface — grid rows,
drafts, capture targets, naming expressions, providers — sees a single
logical totals field. The reader applies Picard's load rule (primary
spelling wins when a file carries both; the secondary comment stays
byte-preserved), and the prepared-copy FLAC writer applies Picard's write
rule: replaces land under both paired native spellings and removals delete
both, with verification pinned to the primary the reread surfaces.
ADR-0066's "similarity never creates an alias" contract is otherwise
unchanged.

**Trackbench decision (ADR-0075):** a revision-qualified metadata source cache
is durable correctness state, not an audio-file backup and not permanent path
ownership. When no persisted local occurrence owns a relocation target, its
historical cache is atomically removed and replaced by the current source cache
or verified published document. An active target occurrence still blocks.
File executors also report exact descriptor-created and synced directories
before artifact or dependent work can fail, allowing remaining batch members
to use them after a source rollback while unexplained appearances still fail
closed.

### Plan pipeline

```text
selected TrackRefs
  -> evaluate format expression
  -> validate/sanitize each path component
  -> resolve normalized absolute targets
  -> detect conflicts and relationships
  -> show immutable lexical plan
  -> fresh non-mutating filesystem preflight
  -> show publication kind and live blockers
  -> journal
  -> execute
  -> update local sources/Trackbench lists/statistics
  -> report/retry/undo
```

The preview must detect:

- multiple sources mapping to one target;
- collisions with existing files/directories;
- source equals target and case-only rename cases;
- targets escaping the chosen root (`..`, absolute outputs, symlinks);
- invalid/NUL/separator characters and reserved names for the chosen filesystem;
- maximum component/path lengths;
- read-only sources or unwritable targets;
- cross-filesystem moves (copy + verify + delete rather than atomic rename);
- multiple logical cue tracks referring to the same physical source;
- companion files that would be copied twice;
- Trackbench-list and statistics consequences.

Sanitization is a named, versioned built-in policy, not hidden string
replacement or user-executed code. Linux and portable policies show raw versus
sanitized output. ADR-0171 rejects arbitrary custom sanitizers; future policies
need a new stable identity and evidence. `$ascii()` remains a language function
but is not a substitute for path safety.

### Execution safety

- Prefer atomic rename when source and destination share a filesystem.
- For cross-filesystem moves, copy to a temporary sibling, flush as appropriate,
  verify size/content policy, atomically publish, then remove the source.
- Never delete a source after a failed or unverified copy.
- Use explicit overwrite/skip/rename/error conflict policy; default to error.
- Journal completed steps durably enough to resume or reverse after a crash.
- Update persisted paths, Trackbench list references, preparation selections,
  statistics, sidecars, and active local-playback references as one logical
  transaction. If a destination happens to be an MPD music root, a database
  update is a separate explicit command in the MPD authority; it is never part
  of or inferred by the local publication transaction (ADR-0058).
- If the persistence update fails after filesystem success, keep a
  reconciliation record; do not pretend the operation rolled back when it did
  not.

### Companion files and empty folders

Optional “copy/move source folder content” supports covers, booklets, logs, and
cues. It requires a previewed include/exclude policy. Removing empty source
folders occurs only after successful file operations and must never remove a
watched root or a directory containing excluded/hidden user data.

### Cue-aware behavior

Several selected logical tracks may share one audio file. A physical rename or
move runs once per source, and all logical references follow it. External cue
paths must be rewritten safely when chosen. Renaming individual cue tracks into
separate files is a conversion/split operation, not a filesystem rename.

## Metadata write transaction

For each physical source:

1. Acquire a per-source mutation lock.
2. Revalidate source identity/revision.
3. Read a complete native metadata/container representation.
4. Apply the staged logical patch through the format adapter.
5. Write via the safest supported strategy—temporary sibling and atomic replace
   where possible, or the format library's proven in-place update.
6. Preserve permissions and requested timestamps; fsync policy is configurable.
7. Re-read and validate intended fields plus preservation invariants.
8. Update caches and dependent views.
9. Retain journal/backup information according to undo policy.

Batch concurrency must serialize operations that touch the same physical source
even if they originated from different logical tracks.

## Sidecars

Sidecars solve otherwise unwritable formats and user preference not to modify
masters. They must be portable and explicit, not an opaque database accident.

Proposed sidecar requirements:

- one versioned file per physical source or a clearly defined directory index;
- source URI plus robust identity/fingerprint;
- logical subsong identifiers;
- typed multi-value metadata, artwork references, and loudness provenance;
- atomic writes and human-inspectable serialization;
- automatic following during Trackbench file operations;
- conflict behavior when source and sidecar are changed independently;
- opt-in export/import and cleanup.

The loudness slice is decided and implemented by ADR-0141: one strict
versioned JSON file per physical source (`<file>.tkmeta`), identity by
decoder selection + sample range, staleness by audio size+mtime, atomic
revision-gated merges, stale entries dropped wholesale. General
metadata/artwork payloads, file-operation following, and export/cleanup
remain open extensions of the same schema.

Trackbench maintains no local library database for now (ADR-0058); a future
local index may cache the same data for speed but is not the only copy when the
user expects portable persistence.

## Tests

Every writable format needs real fixture round trips that assert:

- intended fields changed and multi-values survived;
- arbitrary/unknown frames or atoms survived;
- audio essence is unchanged for metadata-only operations;
- artwork and ReplayGain not targeted by the operation survived;
- padding/size changes are acceptable;
- invalid encodings and malformed-but-readable files fail safely;
- cancellation and injected write failures leave a valid original or recoverable
  journal;
- external changes are detected;
- cue/shared-source operations occur once.
