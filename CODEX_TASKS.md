# Trackbench: agent task list

## Post-release priorities started on 2026-09-21

Branch: `feature/listening-discovery-autoplaylists`. These are the first three
priorities from the product assessment, separate from the historical task
numbers below. The three priorities are not complete.

1. **Listening history, resume, album shuffle:** local playback now collects
   qualified listens through the persistence worker, with source identity
   preserved across app-managed publications and durable event deduplication
   (ADR-0207, schema 40). Optional local Play count / Last played columns load
   asynchronously (ADR-0208). Melody now collects its own statistics; capability-gated
   server columns and stats invalidation are supported (ADR-0209), separately from
   local history. ADR-0210 adds opt-in paused local list resume and hardens
   Melody's server-owned resume. ADR-0211 extends paused resume to interrupted
   Up Next requests in both authorities. ADR-0212 adds one-shot album-preserving
   shuffle for local lists; ADR-0213 extends Melody support to all lists, including
   search results and the stashed queue. History queries and continuous
   album-random playback remain open.
2. **Command discovery and staged-operation UX:** the curated command palette
   is available with Ctrl+Shift+P, with Settings retaining shortcut editing
   (ADR-0205). Parameter choices such as device names and numeric ratings are
   deliberately excluded. Broader staged-operation UX refinements remain open.
3. **Autoplaylists and library views:** the existing dynamic-rule editor now
   refreshes after local scans and committed index changes, with coalescing and
   stale-query suppression (ADR-0206). Persistent live tabs, preservation of
   playing occurrences during membership changes, and custom grouping remain
   open. Opened snapshots retain their existing semantics.

---

Tasks 1, 2, 3, and 5 are implemented; validation notes and follow-ups are below. The
MPD-mode work this file used to carry is finished — see the ADR trail
0187 → 0188 → 0190 → 0191 → 0192 and the "Shipped" section at the end.

---

# Task 1 — Picard-style covers (Wave 3) — implemented

ADR-0184 records the shipped policy and its limits.

- [x] Covers settings now drive reviewed Apply: embedding, a front-cover folder
  image, or both. Policy is captured with destination revision/hash evidence;
  `ArtworkWritePlanIntent` stays unchanged. The suffix follows image MIME
  (`.jpg`/`.png`) and conversion's cover preference remains independent.
- [x] The Fields pane has a 112-pixel front thumbnail sharing the Artwork
  inventory. Drop, paste, Choose file, and Fetch stage front changes. The
  context menu includes Remove, Open Artwork tab, and Cover settings.
- [x] Artwork remains the advanced surface and honors the same storage policy.
  Folder-only fronts need no qualified embedded writer. Removal remains
  embedded-only; external image deletion is not included.
- [x] Folder publication has schema-38 journal evidence, descriptor-relative
  no-symlink publication, exclusive creation/atomic replacement, retained
  backups, identity/hash revalidation, cancellation, and startup recovery.
  Shared-folder conflicts block review; identical publications are idempotent.
- [x] Real-file and GUI tests cover policy persistence, conversion independence,
  thumbnail drop/paste/fetch staging, explicit review and cancel, folder-only
  and combined saves, unchanged WavPack bytes without an artwork writer,
  create/replace, exact replacement undo, conflicts, cancellation, and
  interrupted publication/prepublication recovery.

Validation (2026-09-20): the complete development build succeeds. All 67 CTest
suites are validated, with the local-library and bench suites rerun successfully
after updating schema expectations and retaining the Artwork table's 60-pixel
previews. The core publication/journal/migration suites also pass after the final
commit-admission check. Changed-line formatting, new-file formatting, SPDX, and
diff checks pass.

Folder and media files have separate journals. A later media failure explicitly
reports an already saved folder cover; retry recognizes identical bytes. Save
folder covers before Rename/Move: this wave blocks combining those actions
instead of silently bypassing folder publication. Container writers remain
embedded-only. Full details and migration boundaries are in
[`docs/adr/0184-policy-driven-front-covers.md`](docs/adr/0184-policy-driven-front-covers.md).

---

# Task 2 — loose ends

- [x] **Artwork test failure.** Reproduced
  `metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes` with
  `role_choices == 0` after `mpdStoredPlaylistTabsFollowServerAuthority`.
  The visible file chooser kept its focused filename field empty despite
  `selectFile()` calls. The test now enters the requested path in that field
  before accepting, retaining the role, staging, commit, and reread assertions.
- [x] **Genre query covering index (Melody).** `../melody/melodyd/db.go`
  transactionally replaces `track_tags(tag, value)` with
  `track_tags(tag, value, track_id)`. The new migration/query-plan test proves
  repeatable upgrade, preserved genre results, and a covering lookup. No live
  server database was changed and no end-to-end latency improvement is claimed.
  The substring search is unchanged as requested.
- [x] **Duplicate playlist-tab restore.** Scratch-list responses now refresh
  sidebar/chrome without calling the playlist-name handler and restoring tabs
  again. The UI regression covers both marking and unmarking scratch lists.
- [x] **Dangling ADR-0189 comments.** Both references now point to ADR-0188.

Validation (2026-09-20): the development bench target builds; the complete
Melody daemon test suite and the bench CTest suite pass. The ordered playlist /
artwork regression passes five consecutive offscreen runs and five consecutive
desktop runs. Changed C++ lines pass clang-format, and the SPDX and diff checks
pass. A full direct desktop run before
these fixes passed the artwork test but exposed three separate failures:
`mpdSearchProjectsControllerResults` (42-pixel cover versus 28),
`metadataDialogLayoutsPersistAsynchronously` (1360-pixel height versus 640), and
`musicBrainzIdentifyStagesChosenVersion(untagged-manual-mapping)` (drop mapping).
These desktop-specific failures remain separate follow-ups; offscreen CTest
passes.

Development rollback for the Melody index migration (with the daemon stopped):

```sql
BEGIN;
CREATE INDEX IF NOT EXISTS idx_track_tags_tag_value ON track_tags(tag, value);
DROP INDEX IF EXISTS idx_track_tags_tag_value_track;
COMMIT;
```

Run the older daemon after rollback; the updated daemon reinstalls the covering
index at startup.

---

# Task 3 — Last.fm scrobbling and loved tracks — implemented, bounded

- [x] Melody-owned account authorization, primary-output scrobbling, Now Playing,
  persistent retry outbox, and advertised `melody_lastfm` commands. Multiple
  clients and outputs do not collect additional listens.
- [x] Local account and scrobbler on a dedicated worker; shared Settings page
  selects Local playback or Melody server. Neither MPD client playback nor the
  Melody endpoint is counted by the local scrobbler.
- [x] Single-track Last.fm menu shows loved state and offers Love/Unlove for
  local and server tracks, independently of scrobbling enable. Loved-track
  dynamic playlists use the next explicit refresh and existing library matching.

See [the guide](docs/lastfm.md) and [ADR-0197](docs/adr/0197-authority-owned-lastfm.md)
for authentication, private-file persistence, conservative playback accounting,
retry semantics, and account-switch behavior. Fixture tests cover authorization,
UTF-8/form encoding, Love/loved state, offline restore/retry, playback thresholds,
pause/seek/coarse position reports, output ownership, and UI authority selection.
Real Last.fm account authorization and provider-side playlist propagation remain
manual validation; tests never submit listening history to a real account.

---

# Task 4 — Active queue Auto-DJ — TODO

- [ ] Add an explicit toggle for automatically appending related tracks to the
  active playback queue, using the current queue as the recommendation seed.
- [ ] Support both local and MPD/Melody playback through shared recommendation
  logic and authority-specific library matching and queue adapters.
- [ ] **Proposal:** replenish a small upcoming buffer as playback progresses;
  use Last.fm similarity and/or library tags/ratings, avoid queued tracks and
  recent repeats, and preserve manually queued tracks and their order.
- [ ] **Proposal:** turning the toggle off stops pending additions without
  removing tracks already added. Discard stale recommendations after queue or
  playback-context changes, and show when no suitable library matches exist.

Record refill thresholds, seed selection, recommendation sources, and ownership
(Melody daemon versus client) in an ADR before implementation. Auto-DJ extends
the active playback context, not an unrelated browsed playlist or inactive queue.

---

# Task 5 — Up Next request queue — implemented

- [x] Temporary FIFO over normal playback: finish the current track, play
  requested tracks, then resume the original context automatically.
- [x] Queue next / Queue at end actions and a transport-linked Up Next panel
  with flat rows, reorder/remove/clear, Undo, and return-now.
- [x] Shared interaction and transition contract locally and in Melody;
  Melody owns remote progression and persistence independently of the client.
- [ ] Coordinate with Auto-DJ when Task 4 is implemented: manual requests take
  precedence; recommendations replenish normal playback, not the request FIFO.
- [x] Regression coverage for duplicate occurrences, FIFO return, Consume,
  Single/Repeat, shuffle, edited return contexts, persistence, stale revisions,
  local gapless handoffs, and closing the source tab during requests.

See [the guide](docs/up-next.md) and [ADR-0196](docs/adr/0196-up-next-request-queue.md)
for implementation boundaries and the limited stock-MPD fallback. Validation:
seven targeted Trackbench suites passed, including real local audio and workspace
integration; Melody's `go test ./...` passed. Both development binaries built.
Melody was subsequently deployed to gemenon and restarted successfully; the user
confirmed Up Next works. Every output backend has not been exercised.

---

# Task 6 — Dynamic playlists from selected tracks — TODO

- [ ] Add **Create dynamic playlist…** to track context menus in local and
  MPD/Melody views, honoring the existing multi-selection when right-clicked.
- [ ] Open the shared dynamic-playlist editor with the selected tracks as seeds,
  in the originating library authority, ready to name, preview, and save.
- [ ] **Proposal:** offer Similar tracks via Last.fm and editable rules derived
  from selected metadata (artists, genres, ratings). Show the derived criteria
  before evaluation; do not silently turn every selected tag into a constraint.
- [ ] Extend the current single-track similarity definition to support multiple
  seeds with bounded requests, balanced candidate merging, duplicate removal,
  and the existing refresh-variety policy. Preserve exact local/remote source
  identities and version saved definitions explicitly if their schema changes.
- [ ] Validate single/multi-selection, missing seed metadata or credentials,
  cancellation, saved-definition round trips, and both library adapters.

Record seed-combination and rule-generation semantics in an ADR before
implementation. This creates an editable definition; it does not automatically
modify playback or enable Auto-DJ.

---

# Task 7 — Dynamic playlist tab interaction parity — TODO

- [ ] Make dynamic playlist results behave like ordinary track tabs for
  applicable right-click actions, playback, now-playing indicators, selection,
  keyboard navigation, drag/drop, ratings, and Up Next requests.
- [ ] Reuse shared track actions and playback identity tracking for local and
  MPD/Melody results instead of maintaining a reduced preview-only interaction.
- [ ] Keep result membership owned by the definition. Specify whether manual
  removal creates an exclusion or requires opening an editable snapshot; do
  not silently rewrite rules or let the next refresh undo an apparent edit.
- [ ] Preserve selection, scroll position, and playing markers across refreshes;
  distinguish source identity from duplicate playback occurrences.

---

# Task 8 — Direct Melody tag editing and server-side metadata — TODO

Direct mapped editing is implemented in ADR-0203; server-side metadata and
writes remain proposals.

- [x] With a configured music-path mapping, resolve the selected files and open
  the standard editor tab and sidebar file list directly, without creating an
  intermediate local queue tab. Keep local file mutation explicit and separate from MPD
  queue operations; reuse the existing preview, conflict, and recovery workflow.
- [ ] Add a capability-gated Melody operation that reads actual file metadata
  on demand on the server and returns a complete editing snapshot, including
  arbitrary/multi-value tags, artwork inventory, relevant container information,
  and file revision evidence. Do not substitute the indexed library metadata.
- [ ] Load snapshots asynchronously with cancellation and bounded concurrency,
  avoiding full metadata reads through a potentially slow network mount.
- [ ] Revalidate the actual file against the snapshot before Apply; refuse stale
  edits and preserve unknown tags and container data. Specify revision evidence
  usable across server reads and mapped client writes before implementing.
- [ ] Refresh affected Melody library entries after successful saves so lists,
  searches, artwork, and other clients see the changes.
- [ ] **Later proposal:** perform reviewed writes on Melody itself, removing the
  music-mount requirement. Retain explicit commit, conflict detection, progress,
  cancellation, safe metadata preservation, and recoverable journals/undo.

Record the protocol, authority boundary, access controls, and fallback for
stock MPD/older Melody in an ADR before implementation. Test complete real-file
snapshots, concurrent modifications, mapping failures, recovery, and unchanged
playback/tab selection. Server-side metadata reads and writes are not implemented;
the immediate workflow still reads and writes mapped files on the client.

---

# Shipped

Kept short: the reasoning lives in the ADRs, the detail in the commits.

**Lists live on the server (ADR-0191, supersedes 0181/0188's client-owned
tabs).** A working tab is an ordinary MPD stored playlist carrying a
Melody `scratch` flag, which only decides presentation: scratch lists in
the tab strip, curated playlists in the sidebar. Created with
`playlistadd`, edited with the stored-playlist commands, played as a
context. Closing one deletes it; "Keep as playlist" promotes it. Legacy
client-owned documents are dropped at startup, as agreed. This replaced a
design where the client held its own copy of a list, which made a stock
client's `mpc add` invisible and then overwrote it.

**Committed searches are lists (ADR-0192, supersedes ADR-0140's search
tabs).** Search hits are written to the server as a working list named
after the query; re-running the query replaces that list. One kind of
MPD-side tab remains.

**Tabs are destinations (ADR-0190).** The library's direct actions name
the visible tab (Add to X / Replace X); "Send to tab" reaches any other
tab with Add / Insert next / Replace, plus "New list…". Working tabs
accept drops from every server-track surface, and an unexpanded library
branch is fetched before the drop completes.

**Playback contexts (ADR-0187, melody `melody_context`).** Playing a list
materializes it into the single queue and stashes what it displaced, with
per-playlist resume. Stock clients only ever see an ordinary queue. The
Queue tab shows the stashed list while another plays, and edits aimed at
it reach the stash through `melody_context queue*`.

**Melody protocol additions.** `melody_context stage` (a list too long
for one 4 KiB command line arrives in chunks), `melody_playlistadd`
(appends a staged list in one transaction — 100 tracks went from 0.91s to
0.08s), `melody_scratch` (the working-list flag), plus the full filter
grammar and ReplayGain conditions from the earlier waves.

**Bugs fixed along the way.** Deleting a playlist left its tracks behind
(foreign keys were never enabled, so `ON DELETE CASCADE` never fired and
a new playlist reusing the row id inherited them). `find date 1992`
matched any field containing 1992; legacy conditions carried an empty
operator the evaluator read as a numeric compare, and `search` lost its
substring semantics. Server searches asked for a 500-row window. Covers
stopped loading after 64 albums because the request budget counted
finished requests. Delete did nothing in list tabs. The client wedged
after a command libmpdclient rejected locally.

**Two earlier reported bugs (Task 3 in the old list).** The output hover
was describing the local endpoint's decoder mode rather than the server's
ReplayGain mode; it now leads with `Server ReplayGain: <mode>`.
`REPLAYGAIN_ALBUM_GAIN MISSING` matched everything because melodyd stores
gains in `tracks` columns, never in `track_tags`; the four `replaygain_*`
conditions now resolve from those columns in both query paths.

### Cover Apply regression follow-up

- Fixed unrelated malformed recovery records blocking every cover save (ADR-0193).
  Source-scoped admission preserves and reports the damaged record, still blocks
  its source, and permits unrelated media and folder-image publication.
- Added a real-FLAC regression with missing journal child rows in an existing
  workspace database, covering both embedded and folder cover publication.
- Validation: development build succeeded; metadata-commit, operation-journal,
  list-repository, workspace-backup, and offscreen bench-main-window suites passed
  (5/5), plus SPDX and diff whitespace checks.

### Tag editor file-list cleanup

- Replaced the hidden-view/sidebar-mirror arrangement with one file-list widget
  per editor, hosted directly in the sidebar (ADR-0194). Metadata and selection
  models are editor-owned; closing an editor releases its hosted widget.
- Retained padded checkbox selection and relative filenames. UI regressions
  cover two editors with independent selections, tab switching, widget counts,
  destruction, and individual/bulk edits. Development build and full offscreen
  bench-main-window suite passed.

## 2026-09-20: Shared dynamic playlists and small desktop fixes

- Notification focus suppression is optional; Settings provides a test with
  delivery feedback. Runtime failures are visible.
- Library ordering is a dropdown. Melody uses its existing cached latest-album
  response; ordering is cached until library/connection invalidation.
- AcoustID links to application registration. Metadata services also holds the
  Last.fm key and registration link.
- One dynamic-playlist engine/editor supports local index and server adapters,
  saved versioned definitions, tag/rating rules, limits/shuffle, Last.fm sources,
  cancellation, and explicit stable snapshots. See ADR-0195 and
  `docs/dynamic-playlists.md` for boundaries and follow-ups.
