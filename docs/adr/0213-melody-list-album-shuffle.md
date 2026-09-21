# ADR-0213: Album shuffle targets every Melody list

Status: Accepted

## Decision

Extend ADR-0212 to named playlists, scratch/search-result lists, and the stashed
unnamed queue. Lists have equal editing semantics whether or not they are
currently playing. Playback context is not the edit target.

Melody advertises `melody_list_shuffle_albums NAME [REVISION]`. An empty name
targets the visible unnamed queue, including its stash. Reading without a
revision returns an opaque token; applying with that token rejects intervening
changes. Trackbench captures the selected tab's name before queuing the worker
command, reads its revision, and submits the edit without automatic retry.
The server remains the sole owner of ordering and metadata grouping.

Named lists have transactional revision records, initialized by an additive
migration. SQLite triggers invalidate them on all playlist/entry writes,
including ordinary MPD and HTTP operations. Delete/recreate cannot reuse the
revision. Reordering updates existing entry positions in one transaction.
Active list edits remap existing normal queue occurrences, preserving distinct
duplicate IDs, priorities, the current occurrence and offset, and Up Next.
Inactive lists only change stored order and remap their remembered resume row.
Stashed queue edits preserve its priorities and duplicate resume occurrence,
without touching the active named playback context. Database errors roll back;
resume-file persistence errors explicitly report that the edit already applied.

Grouping and limits remain those of ADR-0212. Metadata planning runs outside
the queue lock. No operation reloads or starts the current track. Server edits
still have no client-owned undo. Local list editing and its undo are unchanged.

Older Melody daemons retain the restricted active-unnamed-queue fallback;
stock MPD does not expose an emulated command. Continuous album-random playback
is a separate, unfinished feature.

## Verification

Server tests cover active duplicate occurrences, Up Next, inactive scratch
lists, saved resume positions, the stashed queue, stale revisions after edits,
rename and delete/recreate, and transactional rollback. Client protocol tests
cover explicit named/unnamed targets and revision round trips. UI tests cover
local lists, named Melody tabs, stashed queues, legacy capability fallback,
and unsupported servers.
