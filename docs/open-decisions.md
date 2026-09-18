# Open decisions

Accepted foundations are recorded in ADRs through 0174. ADR-0058 supersedes
ADR-0025's permanent process split: Trackbench is the primary workspace and
hosts authority-bound MPD Queue and Local Queue tabs. The active primary tab
switches the server-library/local-folders sidebar, MPD/PipeWire output selector,
transport controller, row type, and available commands. The queues never mix,
and local operations remain unavailable to MPD rows. The former standalone
Trackknife executable was retired in ADR-0071; stored-playlist tabs were
migrated in ADR-0129 and committed search tabs in ADR-0140.

## Resolved for M2–M3

1. Credentials remain session-only until a desktop secret-service adapter is
   selected; ordinary settings and SQLite never receive passwords.
2. Browse/search pages and artwork are bounded memory caches. Only explicit
   working-list snapshots survive restart, so the MPD authority does not create
   a competing local library index.
3. Playback modes acknowledge optimistically and reconcile against the next
   authoritative snapshot. Stable-ID queue mutations remain pending until MPD
   confirms them; conflicts refresh instead of replaying the edit.
4. The typed MusicBrainz projection and tested deterministic fallback keys own
   release/artist/medium/track grouping. New aliases require corpus evidence.
5. The default visual language is desktop-native Qt Widgets with lightweight
   delegates for compact queue grouping and row states, as accepted by
   ADR-0012.

## Resolved local-engine foundations (pre-split M4 work)

ADR-0014 fixes raw-path preservation, deterministic recursive folder
discovery, duplicate handling, directory-symlink behavior, cancellation, and
recent-location semantics for ad-hoc local sources. ADR-0015 fixes the initial
streaming decode contract at source rate/layout and the decoder-core PCM WAV,
AAC/M4A, LAME MP3, Opus/Ogg, and Ogg Vorbis acceptance fixtures. ADR-0016 and
ADR-0018 fix the bounded playback core and the direct PipeWire adapter.
ADR-0021 fixes the serialized playback worker, ADR-0023 list progression,
ADR-0024 volume/device selection, ADR-0029 named and exact buffer policy, and
ADR-0030 persistent sink/default monitoring with strict explicit-target
pause/recovery, and ADR-0031 exhaustive container-chapter projection through
the shared logical segment model. ADR-0032 adds typed decoder selection,
bounded tracker-subsong enumeration, and an explicit opt-in policy for
alternate container streams. These contracts moved into Trackbench unchanged;
ADR-0058 reuses the distinct MPD and local controllers behind the active
primary tab rather than restoring the ADR-0022 domain chip or a mixed
transport.

## Resolved M5 read, inspection, and draft foundation

ADR-0033 fixes the Qt-free ordered metadata document, deterministic canonical
lookup, provenance precedence, initial MusicBrainz projection, raw-path source
revision, and conservative read-only TagLib property boundary. Trackbench list
snapshots cache effective ordered values but are not mutation authority. At
that point exact format mappings, artwork preservation, sidecars, and journals
remained open. ADR-0034
adds the bounded sparse selection union, exact definitions for common, mixed,
missing, and partial fields, and the non-modal Properties workspace with
per-item exact-value/provenance inspection. ADR-0035 adds bounded sparse
replacement/removal patches, deterministic result-state projection, direct
scalar grid editing, bounded undo/redo, and explicit draft discard. These
drafts intentionally resolve no write claim. ADR-0036 makes the compact
Fields/Original/Draft projection primary, and ADR-0037 adds structured exact
ordered value editing without delimiter parsing. ADR-0038 supersedes the
separate Tracks drill-down with a file list above that table: file selection is
now the single source of individual versus bulk edit scope, with arbitrary
subset summaries projected outside the UI thread. ADR-0039 adds a dynamic,
copy-on-write session field vocabulary plus explicit Add field and Remove field
commands without changing that scope. ADR-0040 adds bounded deterministic field
completion while keeping arbitrary names open, ADR-0041 adds the complete
cancellable in-memory Draft projection, and ADR-0042 adds the physical-source-
aware revalidated write-plan/conflict preview. ADR-0043 proves preservation-
verified native-FLAC text preparation, ADR-0044 adds its journaled commit and
recovery executor, ADR-0045 adds the idempotent all-occurrence source-cache
transaction, and ADR-0046 makes startup recovery evidence visible while adding
bounded retained-backup undo/release. ADR-0047 adds the explicit cancellable
multi-source Apply job, ordered partial results, and fresh-preview retry.
ADR-0048 adds the versioned ordered transformation-chain boundary, its first
literal/remove/per-value/`tkfmt-1` actions, immutable final preview, and one-
transaction draft staging. ADR-0049 adds normalized saved-chain persistence
through the serialized worker plus exact append/copy/split/join semantics.
ADR-0050 exposes first-character Unicode capitalization as a typed saved action
whose remainder-preserving semantics are distinct from title case.
ADR-0051 exposes the saved catalog directly in a persistent checkable tagging
side panel. Checked definitions are composed in displayed deterministic order
against a temporary draft copy before the immutable write plan, so repeated
preview cannot compound non-idempotent actions and Apply remains explicit.
ADR-0052 makes Properties a temporary protected workspace tab instead of a
separately sized window and moves transformation file/step diagnostics beneath
expandable Field/Old/New change rows without changing exact preview semantics.
ADR-0053 fixes case-sensitive complete-value remove/replace semantics and
bounded consecutive numbering in captured file order while explicitly
deferring group resets and `TOTALTRACKS`. ADR-0064 adds exact bounded Unicode-
scalar prefix extraction as an exposed saved action without treating it as a
date parser. ADR-0065 adds a bounded Picard-style paste translator that
discards source after generating typed rules, plus dialect-qualified
conditional removal; it does not settle full chain interchange or external
script compatibility. ADR-0066 rejects separator-derived tag aliases: only an
explicit format mapping creates a semantic field, while freeform native fields
remain independently visible and mutable. It does not settle the typed native
identity required by future ID3v2 or MP4 writers. ADR-0054 fixes the shared
preparation-plan shape and separates reusable relative output layouts from explicit
destination roots so rename/move, ReplayGain, and conversion can share one
review vocabulary. ADR-0055 fixes the persisted schema-1 profile contracts,
exact `linux-v1` sanitization, and the pure lexical planner's revision, alias,
containment, and collision behavior against an explicit observation snapshot.
ADR-0056 fixes no-symlink fresh preflight, actual same/cross-filesystem
classification, and the numeric durable publication boundaries through
dependent-state commit and source removal. ADR-0057 fixes locked
descriptor-relative same-filesystem no-replace publication, rollback, and
startup replay. ADR-0059–0063 add the concrete dependent path transaction,
same-filesystem undo, cross-filesystem execution, active-playback relocation,
and bounded multi-source Apply. ADR-0067 exposes path-only Rename/Move through
one final-metadata/path review, bounded Apply, both startup recovery paths,
unified operation history, and same-filesystem undo. ADR-0073 and migration 21
qualify the journaled single-source destination-artifact core with a real
native-FLAC writer. ADR-0074 and migration 22 compose it into bounded Apply,
atomic all-occurrence metadata/path reconciliation, visible/player refresh, and
published-artifact startup recovery. Cross-filesystem and changed-artifact undo,
portable/custom sanitization and Unicode normalization,
`TOTALTRACKS` numbering totals (grouped counters landed in ADR-0104),
richer match dialects, other exact format writers, artwork
publication/UI, thumbnail presentation, and sidecars were remaining M5 work at
that stage. ADR-0068 separately fixes the `tkcapture-1` grammar, bounded
ambiguity policy, four source kinds, multi-target chain behavior, and schema-20
saved action. ADR-0072 fixes the strict native JSON interchange schema for the
complete current typed chain while excluding catalog identity and automatic
state. ADR-0073 fixes the orthogonal file-publication content intent and
conservative prepared-artifact recovery boundary. ADR-0074 fixes exact
metadata/path pairing, the combined dependent transaction, and the recovery
reread contract without serializing metadata into the file journal. ADR-0075
fixes reused-target cache replacement and exact executor-created directory
evidence after a later source rollback.
ADRs 0080–0161 subsequently qualify the current text/artwork matrix, direct
staged Apply, retry, and recovery workflows. ADR-0169 adds portable naming;
ADR-0170's saved field layouts were retired by ADR-0177; ADR-0178 replaces
them with previewed field-filter scripts. ADR-0171 closes M5 and moves unadvertised
format and policy expansion outside that gate.
ADR-0076 separately fixes the bounded read-only native-FLAC/external artwork
inventory, exact type/role/provenance/source-revision evidence, and content-hash
duplicate linkage without qualifying a picture write.
ADR-0077 fixes the lazy Properties presentation, shared file-selection scope,
64-physical-source UI bound, explicit capability/issues surfaces, and
session-only no-SQLite inventory policy without exposing a picture write.
ADR-0078 fixes native-FLAC embedded-picture replace/remove plan identity,
replacement evidence, and prepared-copy preservation/verification without
adding durable image data. ADR-0079 fixes compact migration-23 artwork
operation/inventory-digest evidence plus unchanged-path publication, restart
recovery, all-occurrence revision refresh, and exact undo without persisting
image bytes or inventory rows. ADR-0080 fixes embedded-row Replace/Remove,
fresh immutable review, bounded cancellable Apply, ordered partial results,
all-occurrence refresh, and preservation-exact native FLAC block rewriting.
ADR-0081 fixes native-FLAC append planning/publication, migration-24 Add
evidence, temp-free embedded-donor Copy, and bounded cancellable no-overwrite
Export. Other container writers and configurable export naming remain open.

## Needed to finish the unified workspace migration

1. Migrate committed search tabs into Trackbench without weakening MPD/local
   authority selection. Stored-playlist tabs were migrated by ADR-0129.
2. Resolved by ADR-0071: connection profiles stay in the shared SQLite store
   Trackbench owns; no shell migration is needed.
3. Resolved by ADR-0071: the compatibility shell was removed rather than
   packaged.

## Remaining cross-milestone decisions

1. Sidecar location/format and precedence relative to embedded metadata.
   Resolved for loudness: ADR-0139 keeps CUE ReplayGain in the sheet
   itself (foobar2000 `REM` convention), and ADR-0141 stores every other
   logical track's loudness in the versioned `<file>.tkmeta` sidecar,
   with playback precedence sidecar → CUE → embedded. Open scope shrinks
   to non-loudness sidecar payloads (typed metadata, artwork references),
   file-operation following, and the explicit unwritable-format fallback.
2. Resolved for the supported surface. ADR-0169 adds `portable-v1` beside
   `linux-v1`; ADR-0171 rejects user-authored sanitizers and requires future
   Unicode normalization to use a new evidence-backed policy identity.
3. Resolved. The true-peak half by ADR-0148 (sample peak stays the
   default, an opt-in policy proposes the oversampled true peak in
   `REPLAYGAIN_*_PEAK`, sidecar plus CSV export record the peak kind);
   the Opus storage half by ADR-0149 (RFC 7845 Q7.8 R128 comments with
   a constant 5 dB reference shift). Only rewriting the `OpusHead`
   output gain itself remains future expert-operation work.
4. Resolved for M5 by the qualified format matrix and ADR-0171. Additional
   adapters remain independently gated expansion.
5. Converter's shipped codec/device presets, resampler quality settings, and
   source-root inference UX.
6. Resolved by ADR-0088: the MusicBrainz client paces one serialized request
   at 1.1 s with an identifying User-Agent and caches responses in the shared
   SQLite store for 14 days within a 10,000-entry bound. AcoustID
   fingerprinting was accepted with an optional `fpcalc` runtime dependency
   (ADR-0096).
7. Shipped destination-profile defaults, including whether an accessible MPD
   music root earns a convenience preset.

## Deferred

- Resolved by ADR-0115: the user requested an optional local library over chosen
  folders. Structured tkq queries are decided and implemented (ADR-0150);
  saved searches, autoplaylists, and logical-track indexing remain
  future work.
- Cross-authority conveniences are resolved: ADR-0112 shipped explicit "Load
  as local files" and "Update this folder in MPD", and ADR-0180 added the
  Edit tags/ReplayGain/Convert context actions that materialize mapped MPD
  selections into a local tab automatically.
- Autoplaylist semantics atop tkq-1 (the dialect itself is decided; ADR-0150).
- Plugin ABI/distribution.
- A transactional Melody import/upload protocol and destination adapter; no
  such extension is currently claimed.
- CD ripping, internet radio, streaming-service integration, and non-Linux
  ports.
