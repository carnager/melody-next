# ADR-0196: Up Next request queue over normal playback

## Status

**Accepted and implemented**, 2026-09-20. Validation and boundaries are recorded below. Post-release work
(M0–M10 are complete). Extends ADR-0187 playback contexts and ADR-0195 dynamic
playlists without changing stored-playlist ownership or mixing authorities.

## Problem

A listener playing an album or playlist wants to request a few tracks, hear them
in order, and then return automatically to normal playback. Inserting tracks
into a playlist changes that list; priorities do not express a separate FIFO
with a return point. MPD documents priority as influencing selection in random
mode, not as this request-queue contract:
[MPD protocol](https://mpd.readthedocs.io/en/stable/protocol.html#the-current-playlist).

## Proposed interaction

Call the feature **Up Next**, keeping the existing Queue/list tabs intact.
The transport has an `Up Next · 3` button showing the number of pending requests.
It opens a resizable right-hand panel within the workspace, with remembered
width/visibility. Opening it neither changes the active tab nor steals playback.
The panel follows the current local/server authority and labels that authority.

```text
Up Next · 3                         Clear   ×
Playing   Soundgarden — Like Suicide

1   Alice in Chains — Don't Follow      4:22
2   Temple of the Dog — Hunger Strike   4:06
3   Pearl Jam — Release                 9:05

Then resume: Superunknown · track 8
[Return to playlist now]
```

Use the shared track-view engine, single-line rows, small optional artwork,
artist/title/duration, restrained selection, and comfortable padding. No album
headers. The playing row is separate from the reorderable pending rows. The
return footer links to the source list; under shuffle it says “Resume shuffled
playback from <list>” rather than promising an undetermined track.

Track, album, library, search, and dynamic-playlist results expose:

- **Queue next**: insert the selected block at the front of pending requests,
  preserving selection order; do not interrupt the current track.
- **Queue at end**: append to the request FIFO. This is the default enqueue
  action and the default drop on the Up Next button.
- Existing **Add to <list>** remains normal list editing. Existing positional
  “Add next” is relabeled **Insert after current in list** where retained, so
  it cannot be mistaken for a request with automatic return.

The panel supports drag reorder, exact-position drops, Delete to remove,
keyboard move-up/down, Clear pending, and configurable command-palette actions.
Enqueue keeps selection and focus in the originating view, with a brief
“Added 3 to Up Next” acknowledgement and Undo. Explicit duplicates are allowed
and have distinct occurrence IDs. A drop from the other authority is rejected.
No action uses a bare ambiguous “Add to queue” label for both destinations.

## Playback contract

Example: normal playback is `A → B → C`; A is playing. Enqueue X and Y:
`A → X → Y → B → C`. The original list remains unchanged. Queueing B explicitly
creates an extra request occurrence; its original list occurrence is retained.

- Requests begin at the next eligible track boundary, or immediately when the
  user presses Next. Enqueue alone never starts stopped playback or unpauses.
- Capture the normal continuation once when the first request takes over:
  authority, stable context/list and occurrence identity, and traversal state.
  Further requests do not nest return points. Resume at the next normal track,
  not at the start of the track which finished before the detour.
- Requests play FIFO once each, independently of the normal list's random,
  repeat, and consume settings. Normal traversal remains suspended and its
  settings are preserved. Consume applies to normally completed base tracks;
  playing an extra request does not consume its source-list occurrence.
- Explicit Stop and Stop after current still win. Single mode retains its
  stop-at-boundary behavior; repeat-current cannot starve pending requests.
  Pausing retains the active request and position. Play after Stop follows the
  existing restart/resume policy, retaining pending requests and the return
  point. These precedence rules must be shown in mode tooltips.
- Next skips the active item, then chooses pending requests before normal
  traversal. Previous restarts the active request; it does not put completed
  requests back into the FIFO. Full playback-history navigation is separate.
- Removing/clearing pending requests lets the playing item finish and then
  resumes normally. **Return to playlist now** explicitly skips the active
  request, clears pending requests, and resumes the saved continuation.
- Explicit Play on another normal list/track ends the current detour and clears
  pending requests, with an acknowledgement and Undo for the pending entries
  only. Merely browsing another tab has no effect. Explicitly playing a pending
  row promotes it to the front and starts it; other pending rows retain order.
- If the return occurrence is deleted, resolve the next surviving occurrence in
  the captured traversal, accounting for authoritative edits. If the source
  context itself is gone or no continuation remains, stop visibly; never start
  an unrelated list. New normal-mode changes apply on return.
- An unavailable request shows a playback error and stops advancement, preserving
  the remaining queue and return point. Skip/Remove explicitly continues; no
  silent failure loop. Empty normal playback may still have requests: after
  their explicit start and completion, stop when the FIFO empties.

## Ownership and implementation boundaries

One shared Qt-free request-queue model and transition contract serves the local
adapter and MPD projection. It owns stable request IDs, order, revisions, and
command/event types; the reusable panel only renders state and submits commands.
The remote adapter never runs a competing client-side progression scheduler.
Since Melody is a separate Go daemon, share protocol fixtures and behavioral
traces across implementations rather than claiming shared C++ runtime code.

### Local playback

Integrate arbitration before choosing/prebuffering the next logical source in
the existing playback worker. Requests carry raw-path-safe local source and
segment identity; the return token addresses a stable list occurrence, not a
row number or path alone. Verified file moves update request and continuation
references with other local playback references. Closing a source tab retains
the minimal playback context until the detour ends.

Persist pending requests and continuation transactionally on the existing
persistence worker with an explicit reversible migration. Restore without
starting audio unless the existing resume preference calls for it. At a crash
boundary, restore the active request as resumable; do not claim exactly-once
listening or scrobbling. Scrobbling follows actual playback events.

### Melody

Melody owns the FIFO, continuation, persistence, and end-of-track arbitration.
It keeps working when Trackbench disconnects, and all clients see the same
state. Extend the playback-context machinery; do not implement the detour as
an ordinary saved playlist or overwrite the normal context's resume record.

Proposed advertised capability: `melody_upnext`. Provide snapshot, ordered batch
append/prepend, remove, reorder, clear, play-request, and return operations.
Responses identify request occurrences, base context, active request, pending
order, revision, and a server epoch. Mutations carry expected revision;
conflicts refresh and report rather than overwrite concurrent edits. Bound and
stage large batches before atomic publication. Notify via a capability-defined
idle event plus normal player/playlist events where their projections change.
No automatic replay of ambiguous non-idempotent mutations on reconnect.

The daemon must define a coherent standard-MPD queue/status projection: active
requests have real, distinct song IDs addressable by stock clients, while the
base context retains durable occurrence identities across materialization.
Never report a current song absent from the projected queue. Standard Next,
Stop, playid, deleteid, clear, and context switches must feed the same state
machine. An external explicit play abandons the detour just like the UI action;
external clear stops and clears the active projection without later resurrecting
it. Projection and duplicate-ID fixtures are an implementation gate, not an
assumption that existing context stashing already handles this feature.

### Stock MPD boundary

**Proposal:** full Up Next is available with local playback and the advertised
Melody extension. Stock MPD retains **Insert after current in list** with its
existing semantics and a short capability explanation in the Up Next panel.
Do not present priorities as equivalent or change random/consume behind the
user's back. A client that watches track-end events and rewrites the server
queue cannot promise uninterrupted, multi-client-safe return after disconnect.

**Unknown / separate follow-up:** a stock-MPD assisted mode could be explored,
but must explicitly document its connection lifetime and concurrent-client
limitations. It is not a prerequisite for the reliable local/Melody feature.

## Auto-DJ integration

Manual requests always take precedence. Auto-DJ replenishes the normal upcoming
context, not the manual FIFO, so recommendations cannot postpone a request or
prevent returning. Its seeds remain the normal context by default; the temporary
requests do not silently change its taste. Show the Auto-DJ toggle beside the
normal-continuation footer, visibly distinct from the pending request count.
Reject generated additions captured for a stale context or disabled toggle.

## Validation and rollout

1. Shared transition fixtures: A/X/Y/B example, prepend versus append, duplicate
   source occurrences, Next/Previous, pause/stop, all playback modes, deleted
   return targets, explicit context switch, errors, restart, and Auto-DJ.
2. Local worker integration with real gapless fixtures and request edits near
   a prebuffer boundary. Acknowledged edits define the next transition; if an
   output transition is already committed, show the actual next effective slot.
3. Melody persistence/protocol and multi-client tests, including stock-client
   mutations, daemon restart, stale revisions, ambiguous replies, and disconnect
   during a request. Test every enabled output's transition behavior.
4. Shared panel, menu/keyboard/drop parity, authority switching, flat rendering,
   and inaccessible-capability state. Acknowledge UI commands within 50 ms;
   network, SQL, source resolution, and decoding stay off the UI thread.

Ship local and Melody behavior against the same contract before marking parity
complete. Implementation evidence is recorded below before the feature matrix marks
the feature implemented.

## Implementation record — 2026-09-20

The local/Melody implementation now follows the core FIFO-and-return contract.
The original sections above record the design; these details qualify the first
implementation where the proposed mechanics differ:

- `audio::RequestQueue<Source>` is the Qt-free local occurrence container.
  Melody implements the same transition behavior in Go. One shared Qt panel
  consumes local rows or a typed remote `RequestQueueState`; it never mixes
  them or schedules remote advancement.
- The local audio service publishes opaque current/next occurrence tokens.
  Identical source paths therefore remain distinguishable across committed
  gapless handoffs. Late edits cannot accidentally dequeue another occurrence.
- Melody projects requests as temporary, real MPD queue occurrences. Normal
  traversal filters them out; completion removes only the temporary occurrence.
  Stored-playlist edits retain requests and surviving base occurrence IDs.
  Snapshot/edit commands use the durable queue revision rather than a separate
  epoch. Restore increments that revision. Mutations are never automatically
  retried after an ambiguous disconnect.
- Local persistence uses a version-1 payload in the existing transactional
  `ui_state` repository through `ListPersistenceService`, not new SQL tables.
  No database migration is needed. Removing `playback/up-next/v1` rolls back
  only this feature's local state. Paths are base64-encoded raw bytes; segment,
  source selection, and metadata provenance survive restoration. An interrupted
  request is replayable from its beginning, with no automatic playback. The
  normal shuffle cycle is reconstructed on restart rather than serialized.
- A source tab closed during local requests retains only its model and context,
  with no phantom view. It remains playable for the session; deliberately closed
  lists are not resurrected at restart. A missing restored context stops after
  requests instead of choosing another list.
- Melody's saved queue has additive `requests_version: 1`, `requests`, and
  `queue_ids` fields. Downgrading requires clearing Up Next first so an older
  daemon cannot mistake projected request rows for ordinary list members. Saves
  use a synced temporary file and atomic rename, followed by directory sync;
  request edits report persistence failures.
- Pending edits offer one-step Undo until progression invalidates it. The UI
  supports explicit start of a pending request, return-now, ordered drops,
  Delete, move buttons, and a transport shortcut. Queue membership remains
  independent of the future Auto-DJ feature.

Validation: all seven targeted Trackbench suites passed (local-library-filter,
mpd-client, mpd-session, local-audition, mpris-service, local-library, and
bench-main-window). Workspace tests play real WAV files through duplicate
request occurrences and normal return, including Consume and a closed source
tab. Protocol fixtures cover stale revisions and typed request snapshots.
Melody `go test ./...` passed, including request progression, persistence,
external mutations, stored-playlist edits, and playback modes. Both binaries
built; the panel was checked offscreen. Live daemon deployment, every output
backend, exhaustive concurrent-client timing, and performance budgets have not
been validated by this change. Auto-DJ remains a separate TODO.

## Follow-up: remove obsolete track-selection queue actions

User decision, 2026-09-20: remove the old Add selection next / Append selection
to queue actions and their live-queue equivalents in playlist track tabs. They
mutate the materialized MPD list and duplicate the intent now served by Up Next.
Track menus use Queue next / Queue at end for temporary requests; Send to tab
remains the explicit destination for ordinary list edits. Stock-MPD library
fallback actions remain positional operations, not a client-owned request queue.
