# Trackbench: agent task list

One open task (Task 1), plus loose ends worth picking up (Task 2). The
MPD-mode work this file used to carry is finished — see the ADR trail
0187 → 0188 → 0190 → 0191 → 0192 and the "Shipped" section at the end.

---

# Task 1 — Picard-style covers (Wave 3)

Waves 1 and 2 are done (ADR-0183 compact tag editor, ADR-0185 settings
screen, ADR-0186 Actions menu). The Covers settings page already records
the policy keys (`artwork/embed`, `artwork/write-folder-image`,
`artwork/folder-image-name`, `artwork/fetch-source`) — nothing reads
them at apply time yet. That is this wave.

1. **Folder-cover write capability (the biggest chunk).** Artwork writes
   are embedded-only today by design (`container_artwork_writer.cpp`
   clears external patterns; ADR-0160: external images are never
   modified). Add a journaled "publish folder image" operation: write the
   policy-named image into the track's directory (create or replace),
   integrated into the artwork apply pipeline
   (`operations/src/artwork_apply.cpp` plus a commit path beside
   `operations::commit_artwork_source`; the container writer stays
   embedded-only). Needs **ADR-0184**, superseding ADR-0160's boundary for
   exactly this policy-driven file, following the journaled
   same-filesystem publication rules (ADR-0056/0057 family). Update
   `docs/metadata-and-files.md:218-220` and `:676-684`. Prefer deriving
   the folder write from policy at commit time so
   `ArtworkWritePlanIntent` stays unchanged.
   Note: `settings_dialog.hpp:20` and ADR-0185 already cite ADR-0184 —
   writing it closes those dangling references.
2. **Editor thumbnail.** Small cover widget (~96-128 px) on the Fields
   pane showing the selection's front cover (reuse
   `MetadataArtworkSection` inventory plumbing). Drop/paste an image or
   click Fetch → stages add/replace-front intents automatically per
   policy (embed and/or folder image). Context menu: Fetch cover, Choose
   file…, Remove, Open Artwork tab, Cover settings… (opens the
   SettingsDialog Covers page).
3. **Artwork tab stays** as the advanced surface (roles, per-file rows,
   CAA browser, problems), also honoring the folder-image policy on apply.
4. Tests: policy round trip; thumbnail staging produces the expected
   intents; folder-image journaled write/replace/failure recovery; the
   convert dialog's separate `convert/embed-artwork` carry stays
   untouched.

---

# Task 2 — loose ends

Small, independent, each found while shipping the list work.

- **Pre-existing test failure.** `BenchMainWindowTest::
  metadataPropertiesArtworkRemoveReviewsAppliesAndRefreshes` fails when
  the bench binary is run directly (`role_choices` is 0, expected 1) but
  passes under `ctest`, so it is order- or state-dependent. Verified
  failing on an unmodified tree — not caused by the list work. Nobody has
  looked at it.
- **Genre and substring queries are slow** (melody). `find genre "Rock"`
  takes ~1.7s against 66k tracks: the `track_tags` subquery is not
  covering. A `track_tags(tag, value, track_id)` index would take most of
  that off. `search title "…"` (~1.1s) is a substring match no index can
  answer — leave it unless it bites.
- **Duplicate playlist-tab restore.** `--debug` shows
  `restoring playlist tabs …` twice per connect. Harmless, but it means
  the refresh runs twice.
- **ADR-0189 does not exist.** `bench_mpd_playlists.cpp:502` and
  `bench_main_window.hpp:493` cite it for the expandable playlist
  sidebar; that decision was recorded in ADR-0188 instead. Either write
  the ADR or repoint the comments.

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
