# Trackbench: agent task list

Active handoff task below. (The former M5-era backlog this file carried is
complete — see MILESTONES.md and ADRs 0169-0175.)

This plan spans TWO repositories: Part A lives in ../melody (see its copy at
docs/playback-contexts-plan.md there) and MUST ship and deploy first; Part B
is this repository. Keep each repo green independently.

---

# Playback contexts: MPD mode feels like local mode

## Context

User goal: MPD mode should feel like local mode — every server list independently playable (double-click a row, that list plays from there), switching lists destroys nothing, per-list resume. Hard constraint: melodyd stays 100% MPD compatible — contexts are a capability-advertised Melody extension implemented **over the single queue**: playing a stored playlist materializes it into the live queue while the server stashes the previous queue+position and keeps per-context resume positions. Stock clients always see one consistent ordinary queue (currentsong always in the queue). User-confirmed: full playback contexts; Trackknife's client-owned server list tabs (ADR-0181) become stored-playlist-backed and the client-owned kind retires via migration.

Melody ships first (Part A), then Trackknife (Part B). Each repo stays green independently.

## Part A — Melody (/home/carnager/Code/melody, main)

### Wire protocol — one advertised command family `melody_context`
- `melody_context` → `context: <name>` (empty = live-queue context). No new status keys.
- `melody_context play <name> [pos]` — materialize playlist; pos given → start there elapsed 0 unpaused; pos omitted → resume `ctxPositions[name]` (pos clamped, elapsed best-effort), default 0/0; always unpauses. ACK errNoExist/errArg.
- `melody_context queue [pos]` — restore stash whenever a stash exists (condition = stash present, not active≠""); active="", stash cleared; pos omitted → stashed pos+elapsed, **preserve pause state**; pos given → start there unpaused. No stash: pos → play-at-pos on current queue; no pos → no-op OK.
- `melody_context queueinfo` — stash present: stashed queue in listplaylistinfo shape (`writeTrack(track,-1,0)`, no Id/Pos); else live queue.
- Idle on every switch: `SubPlaylist` + `SubPlayer` + new `SubContext = "context"` (mpd.go:17-25, SubRating precedent).

### Semantics table (normative, goes into protocol.md)
State: `activeContext string`, `ctxStash *{songs,prios,pos,elapsed}`, `ctxPositions map[name]{pos,elapsed}` — all under playQueueMu. Invariant: stock commands never mutate context state.
- play P from queue-context, no stash → stash live queue{songs,prios,curPos,elapsed}; materialize; active=P.
- play P with orphan stash (after rm/clear of previous active) → do NOT re-stash; materialize; active=P.
- play P2 while active P1 → `ctxPositions[P1]={curPos,elapsed}`; stash untouched; materialize P2 (P2==P1 allowed = re-play at row).
- queue-context restore → if active P record its position; restore per rules.
- Queue edits (add/delete/move/prio/shuffle/clear/consume) while active P → operate on materialization only, never written back, lost on switch (MPD `load` semantics); `clear` keeps active=P + stash restorable.
- `playlistadd/playlistdelete/playlistmove` on the ACTIVE playlist → mirror the same structural edit incrementally onto the live queue (songids preserved; delete-of-current advances like queue delete-current; version bump + playlist notify). Non-active playlists unchanged.
- `rename P→Q` active → active=Q, ctxPositions key follows. `rm P`/`playlistclear P` active → active=""; materialized content keeps playing; stash retained (restorable via `queue`).
- Track end/advance/restart: unchanged — advancement is purely positional (main.go:1100/:878); restart restores activeContext/stash/ctxPositions from playqueue.json; curQueuePos+elapsed via existing playstate machinery.

### Steps
- **M1** Lift `replaceQueueLocked(songIDs, prios)` from `addSongsWithPriority`'s "replace" branch (main.go:1531-1552) and fix its verified leak: clear `pendingNextPos=-1`, `prioReturnPos=-1`, `prioPlayedIDs=nil` (mirror cmdClear mpd_commands.go:953). Regression test: replace after prio jump clears prio bookkeeping.
- **M2** Context state on app struct (main.go:145-192) + persistence: `savedQueue` (main.go:1656) gains omitempty `ActiveContext`, `Stash{Songs,Priorities,Pos,Elapsed}`, `ContextPositions map[string]{Pos,Elapsed}`; wire savePlayQueue/restorePlayQueue (legacy files both directions safe).
- **M3** New `melodyd/context.go`: `switchToPlaylistContext(name,pos,hasPos)` / `switchToQueueContext(pos,hasPos)` per table. Mechanics: read `time-pos`/pause via target **before** taking playQueueMu (cmdStatus:160 lock-order model); materialize via `playlistTrackSongIDs` (db.go:1392) + replaceQueueLocked; planSyncTarget under lock, execSyncPlan after unlock; elapsed via `setProperty("time-pos",…)` after sync (cmdSeek:592 model; agents exact via agentPlayAt seek, mpd.go:975); pause-preserve via plan.startPaused; savePlayQueue in every mutation. Elapsed resume is best-effort (same class as 5s watchPlayState poll) — document.
- **M4** `cmdMelodyContext` in mpd_commands.go (read/play/queue/queueinfo; queueinfo reuses playlistTracks/writeTrack path of cmdListPlaylistInfo:2151, stash IDs through the cmdPlaylistInfo track-lookup helper); register `"melody_context"`. Active-playlist hooks in cmdRm:2236, cmdRenamePlaylist:2326, cmdPlaylistAdd:2263, cmdPlaylistDelete:2346, cmdPlaylistMove:2370, cmdPlaylistClear:2396. `SubContext` in mpd.go.
- **M5** Go tests (newQueueStateTestApp harness with temp PlayQueueFile/PlayStateFile; dispatchCapture/dispatchError), 17 cases: read default; play materializes (content/curPos/version/active); play at pos + invalid pos/name; play stashes queue + clears prio state; playlist→playlist stores position, stash untouched; queue restore resumes pos/prios, records ctxPositions; queue restore at pos; queue no-stash no-op / pos-acts-as-play; P→queue→P resumes stored position; persistence round trip + legacy file loads; active-playlist edit mirrors queue (survivor songids preserved); delete-current advances; rm active keeps playback+stash (queue still restores); rename follows; orphan-stash play does not re-stash; queueinfo lists stash else live; queue edits not written back; clear keeps context+stash; resume pos clamped after playlist shrank.
- **M6** Docs: protocol.md "Playback contexts" section (wire + table + stock-clients guarantee); protocol-roadmap.md (this ships the independent-lists capability; Phase 5 ordered sub-queue stays future); CHANGELOG. Then commit/push, merge to main, **deploy to gemenon** before Part B testing.

## Part B — Trackknife (/home/carnager/Code/trackknife, main)

- **T1 mpd core**: `PlaybackStatus` += `song_position` (verified missing — projection.cpp:383 parses songid but never `song`; add parse). Client (albumrate 5-layer precedent, client.cpp:1737/:1755): `melody_active_context()`, `melody_context_play(name, opt pos)`, `melody_context_queue(opt pos)`, `melody_context_queue_tracks()` (reuse stored-playlist track parser client.cpp:966), `move_in_stored_playlist_batch(name, moves)` via command list (:1670-1712 precedent). Session: snapshot += `active_context` + `queue_context_tracks` (filled when non-empty); kinds `context_play`/`context_queue`/`stored_playlist_move_batch`; atomic `melody_context_supported` (pattern session.cpp:161/556); refresh fetches context when supported; refresh masks: context ops → IdleEvent::queue|player (mirror stored_playlist_load :521), batch move → 0U. Fake-server tests: wire bytes, empty-context parse, status `song` parse, capability gating, snapshot fields, masks.
- **T2 controller/model**: `supportsPlaybackContexts()`; invokables `playStoredPlaylistContext(name,row)` (−1 = resume), `playQueueContext(row)`, `moveStoredPlaylistItems(name,rows,insertion_row)`. Degradation without capability: **replace-and-play-at** (clear+load+play(row)), not append. `applySnapshot` stores `activeContextName()` + signal; `MpdQueueModel::setCurrentRow(opt<int>)` honored by CurrentRole (mpd_queue_model.cpp:235) for playlist-tab highlight, driven by active-name==tab-name ∧ status.song_position. Model test for setCurrentRow/CurrentRole precedence.
- **T3 playlist tabs** (bench_mpd_playlists.cpp): activation/double-click (:172/:180) → playStoredPlaylistContext(name,row) with fallback; context-menu "Resume playlist" (row −1). Highlight wiring in acceptMpdStoredPlaylistContents (:482) + active-context changes; cleared on non-active tabs. Lift single-row reorder guard (:188-193) with the batch move (client-side index bookkeeping; single-move path kept for 1 row). Fold-in fixes: removeSelectedRows (bench_list_tabs.cpp:1823) routes by tab kind (today it hits the live-queue selection from playlist/search tabs); playlist-creating edits call browseStoredPlaylists() on success (controller :1804-1806) instead of relying on idle.
- **T4 queue tab as the "" context**: when active context non-empty, the Queue tab shows `queue_context_tracks` (the stash) read-only — queue-mutating actions disabled, double-click → `playQueueContext(row)`, "Resume queue" action; empty context → today's live-queue behavior. This makes "switching lists destroys nothing" visible.
- **T5 MpdListTab migration (retire ADR-0181 kind)**: keep `ListKind::mpd` enum+validator (old DBs load); restoreLists (bench_list_tabs.cpp:191-201) routes mpd docs to a pending-migration queue (no tabs offline). On first connect (hook bench_mpd.cpp:634-643): collision-suffix name against sidebar, push via addToStoredPlaylist, **read back to verify, only then** delete local doc, openMpdPlaylistTab(name). Remove ADR-0181 surface: bench_mpd_list_tabs.cpp (whole file), branch sites bench_list_tabs.cpp:1382/:904/:1137/:1157/:1178/:1313, bench_track_views.cpp:332, bench_mpd.cpp:588/:640, collectDocuments mpd branch :243-287, mpd-list layout bindings :381-389, MpdQueueModel client-edit methods, createServerListTab seam; replace test serverListTabsPersistAndRenderOffline (:3810-3890) with a migration test. Copy-to-server-list menus retarget stored playlists ("New list…" = playlistadd to new name + immediate sidebar refresh). ADR-0180 sugar stays on playlist tabs.
- **T6 open-tab persistence** (roadmap.md:34-36): QSettings QStringList `mpd/open-playlist-tabs` updated on open/close/rename; replayed on connect, filtered against acceptMpdStoredPlaylistNames (:465).
- **T7 tests**: activation issues context_play (name/row); fallback path; queue-tab stash mode (read-only + restore-at-row); highlight follows context+song_position; migration (docs → playlists → docs deleted → tabs open; failure keeps doc); open-tabs round trip incl. rename + vanished name; batch reorder bookkeeping; removeSelectedRows routing regression.
- **T8 docs**: ADR-0182 "Playlist-backed server lists and playback contexts" (supersedes 0181, records migration + fallback); superseded-by note in ADR-0181; update mpd-client.md, melody.md, feature-matrix.md, open-decisions.md, roadmap.md.

## Risks
Lock ordering (elapsed capture before mutex — cmdStatus model); elapsed-seek best-effort on mpv targets (optionally reuse restorePlayState wait loop); switches reshuffle + clear prio bookkeeping (intended, documented); migration guarded by add-verify-delete + collision suffixes; queue-tab read-only mode must be airtight (central tab-kind routing is the guard); bare-idle stock clients see a harmless `context` subsystem name.

## Verification
- Melody: gofmt/go vet/`go test ./...`; live smoke against gemenon after deploy (play context, switch back, restart daemon mid-context, stock client `ncmpcpp`-style probe via nc: status/playlistinfo consistency).
- Trackknife: full dev build; ctest mpd-client/mpd-session/bench-main-window/list-repository (known flakes: localPlaybackModesAdvance, metadataPropertiesArtworkRemove under load); manual: double-click rows across two playlist tabs + queue tab, verify resume + highlight + nothing destroyed.
- Commits: melody first (branch off main, merge+push, deploy), then trackknife on main, push.


---

# Task 2: Settings-driven cover policy and proper settings screen

Wave 1 of the tag-editor compacting is DONE (ADR-0183: side panel became the
"Apply & Scripts" sections tab, footer apply summary, compact header/tool
row). These two waves remain. They are independent of Task 1 above except
for shared wiring in src/bench/bench_metadata_operations.cpp — coordinate if
both run concurrently.

## Wave 2 — proper settings screen: DONE

Shipped as ADR-0185: paged SettingsDialog (General / Naming / ReplayGain /
Covers), profile managers relocated as the reusable
OutputProfilesManagerWidget over the same OutputProfileStore, tag editor
opens Settings via manageOutputProfilesRequested and refreshes selectors on
outputProfilesChanged, ReplayGain prefs read from QSettings at scan time,
and the Covers page records the artwork/* policy keys for Wave 3.

## Wave 3 — Picard-style covers

1. **Folder-cover write capability (new; the biggest chunk).** Artwork
   writes are embedded-only today by design (container_artwork_writer.cpp
   clears external patterns; ADR-0160: external images are never
   modified). Add a journaled "publish folder image" operation: write the
   policy-named image into the track's directory (create or replace),
   integrated into the artwork apply pipeline
   (operations/src/artwork_apply.cpp + a commit path beside
   operations::commit_artwork_source; the container writer stays
   embedded-only). Needs ADR-0184 superseding ADR-0160's boundary for
   exactly this policy-driven file, following the journaled same-filesystem
   publication rules (ADR-0056/0057 family). Update
   docs/metadata-and-files.md:218-220 and :676-684. Prefer deriving the
   folder write from policy at commit time so ArtworkWritePlanIntent stays
   unchanged.
2. **Editor thumbnail.** Small cover widget (~96-128 px) on the Fields
   pane showing the selection's front cover (reuse MetadataArtworkSection
   inventory plumbing). Drop/paste an image or click Fetch → stages
   add/replace-front intents automatically per policy (embed and/or folder
   image). Context menu: Fetch cover, Choose file…, Remove, Open Artwork
   tab, Cover settings… (opens the SettingsDialog Covers page).
3. **Artwork tab stays** as the advanced surface (roles, per-file rows,
   CAA browser, problems), also honoring the folder-image policy on apply.
4. Tests: policy round trip; thumbnail staging produces expected intents;
   folder-image journaled write/replace/failure recovery; the convert
   dialog's separate `convert/embed-artwork` carry stays untouched.


---

# Task 3: Two reported bugs (root causes confirmed on the live server)

Reported by the user. Both were investigated against the running melodyd
on the user's server, including its SQLite database — the findings below
are measured, not hypothesised. Neither is fixed.

## 3a. ReplayGain applies, but the output hover says it is off

Symptom: switching ReplayGain modes against Melody works (playback
audibly gets quieter), yet the transport's output tooltip reads
"Melody ReplayGain: Off".

**Confirmed cause: the mode is only pushed to outputs that are enabled at
the moment it changes, and Trackknife's output is not enabled.** Live
evidence from the server:

- `replay_gain_status` answers `replay_gain_mode: album` — server state is
  correct.
- `outputs` shows `Trackknife` with `outputenabled: 0`, while `caprica` is
  `outputenabled: 1` and primary. The audible gain change happens on
  caprica.
- `cmdReplayGainMode` (`../melody/melodyd/mpd_commands.go:2668-2678`)
  pushes `setProperty("replaygain", mode)` to `a.target()`, the fan-out
  over *enabled* targets only (`main.go:1362`). A disabled endpoint never
  receives it, and nothing replays the mode when an output is later
  enabled or an agent re-registers — the only replays sit on queue-reload
  paths (`main.go:2234`, `main.go:2388-2390`).
- The tooltip reads the endpoint's *local player* mode
  (`src/bench/bench_mpd.cpp:1476-1485` ←
  `melody_endpoint_->snapshot().replay_gain_mode`, set only when the agent
  `replaygain` command arrives, `src/audio/src/melody_agent.cpp:539`,
  `:577`), so a never-notified endpoint honestly reports Off.

Fix shape: melodyd should apply the current mode when an output is
enabled or an agent registers (alongside the volume/queue sync it already
does), not only to the enabled set at change time. Consider also whether
a tooltip labelled as the server's ReplayGain should read
`replay_gain_status` (already parsed for the toolbar button at
`bench_mpd.cpp:335`) instead of the endpoint's player state.

Secondary defect: runtime mode changes are never written back to
`melodyd.toml`, so a restart silently reverts to the file's value. (On
this server the file happens to say `replaygain = "album"` already, which
masks the problem.)

## 3b. `REPLAYGAIN_ALBUM_GAIN MISSING` matches every track

Symptom: the tkq query reports every track as missing the field.

**Confirmed cause (Server library scope): melodyd stores ReplayGain in
dedicated columns and never in the generic tag table, so tag-based filter
conditions can never see it.** Live evidence from the server database:

- `SELECT tag, COUNT(*) FROM track_tags WHERE lower(tag) LIKE '%replaygain%'`
  → **zero rows**.
- `SELECT COUNT(*), SUM(replay_gain_album IS NOT NULL AND replay_gain_album<>0), …`
  → 66803 tracks, 66802 with album gain, 66793 with track gain. The data
  is there, just not as tags.
- The scanner writes these into the `tracks` columns
  (`../melody/melodyd/scanner.go:802-805`), while the filter evaluator
  resolves ordinary tags through `trackFieldValues` → `track["tags"]`
  (`../melody/melodyd/filter_expr.go`), which has no replaygain entry for
  any track. `(replaygain_album_gain == "")` therefore matches everything.

Fix shape (Melody side): teach the filter evaluator — and
`tracksByConditions` for the indexed fast path — to resolve
`replaygain_album_gain`, `replaygain_track_gain`,
`replaygain_album_peak`, and `replaygain_track_peak` from the dedicated
columns, exactly as `rating` and the technical pseudo-fields already are
(`isTechnicalConditionTag` / `matchTechnicalCondition` are the pattern to
copy; `buildTrackMap` already exposes them under `track["replay_gain"]`).
Document them in `docs/protocol.md` alongside the technical conditions.

If the same query also misbehaves in the **local Database scope**, that is
a separate defect with the same shape on the Trackknife side: `MISSING`
compiles to `NOT EXISTS(… local_library_fields …)`
(`src/persistence/src/local_library.cpp`, `exists_head` region ~:615-660),
name canonicalization is ruled out (query and indexer share
`metadata::canonicalize_field_name`, both yielding
`replaygainalbumgain`), so check whether the scanner writes RG fields into
`local_library_fields` at all (inserts at `local_library.cpp:414-433` and
`:1875`):
`SELECT DISTINCT canonical_name FROM local_library_fields WHERE canonical_name LIKE 'replaygain%';`
Per ADR-0166 the index must not answer "missing" from absent evidence —
either index these fields or report a Refresh requirement.
