# ADR-0163: Saved search definitions

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0140, ADR-0150, ADR-0153

## Decision

**Trackbench decision:** Workspace → Search offers named searches with Save as,
Update, Rename, and Delete. Selecting a saved search restores its expression,
query/word mode, and scope, and runs it again. Current-tab scope means whichever
local tab is active when selected, not the tab that originally supplied results.
Library scope queries the cached index; selecting or saving never starts a
filesystem scan. Existing tab-scope technical probing remains demand-driven.

Definitions have stable IDs and explicitly persisted `tkq-1` or `words-1`
dialects. The latter records the existing literal word-search compiler semantics;
future semantic changes need a different persisted dialect. Schema 32 stores
name, expression, dialect, scope, and an optimistic revision. Names are unique
by exact UTF-8 bytes. Limits are 256 definitions, 256 name bytes, and 4096 query
bytes, with the existing compiler's stricter structural bounds still applied.
Both creation and updates validate the query before mutation.

All catalog reads and writes run on one background task at a time per dialog,
using the Qt-free repository API. Updates and deletions compare the captured
revision; stale writers and duplicate names fail without overwriting another
saved search. The UI reports errors and updates its catalog only after a
successful write and reload. Development downgrade refuses to discard nonempty
saved-search storage. Migration runs transactionally with other schema changes.

Editing a loaded query changes only the search input until Update is pressed.
Rename changes only the stored name; Delete removes only the definition. Result
tabs remain ordinary independent snapshots. Automatically maintained autoplaylists
are a separate next step; this change persists no query-backed tab identity and
introduces no background playlist mutation.

Changing input, mode, or scope immediately invalidates prior result actions.
A generation check rejects obsolete worker results, so old hits cannot reappear
under a newly selected saved query. Selecting the same saved search again reruns
it against current data.

## Verification

Repository tests cover reopen/round-trip of both dialects and scopes, update,
rename, deletion, duplicate names, invalid queries/dialects/names, and optimistic
conflicts across two connections. Migration tests exercise downgrade/upgrade and
refusal to discard stored definitions. GUI tests cover save/update/rename,
invalid-update preservation, reopening and reevaluation against changed tab
contents, restoring scope/mode, immediate result invalidation, and deletion.
