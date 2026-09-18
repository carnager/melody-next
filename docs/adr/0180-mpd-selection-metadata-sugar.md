# ADR-0180: File-operation sugar on mapped MPD selections

## Status

Accepted, 2026-09-18. Refines ADR-0112/ADR-0174; supersedes the "deferred
cross-authority convenience" stance of ADR-0058 for the three actions named
here.

## Context

ADR-0112 shipped the music-root bridge as an explicit two-step flow: "Load
as local files" resolves an MPD selection below the configured music folder
into an ordinary local scratch tab, where Properties, Convert, and
ReplayGain behave like any local selection. The original M9 work item wanted
those operations to work directly on MPD-mode selections; that half was
descoped and recorded as deferred (ADR-0058's "local operations remain
unavailable to MPD rows", `docs/open-decisions.md`).

In practice the two-step flow is the common path for a user whose server
library is a locally mounted NAS export: pick tracks in the MPD queue or
server library, load them as local files, then immediately open one of the
three dialogs. The intermediate step is pure ceremony, and Melody's
file watcher closes the loop server-side by rescanning changed files
automatically.

Two implementation facts shaped the shape of the fix. First, every local
file operation is bound to a `ListTab` row, and the journaled metadata
commit refreshes the persisted list document afterwards — a file that is in
no local tab fails with "no persisted list occurrence". True in-place
editing of MPD rows would therefore relax core write-pipeline invariants.
Second, the bench bridge predated the per-profile local music root
(ADR-0174) and still read the global settings key with a naive path join,
bypassing the hardened `mpd::resolve_below_music_root` resolver the rest of
the client uses.

## Decision

The MPD track context menus (live queue, server library tree, stored
playlist tabs, committed search tabs) gain **Edit tags…**, **ReplayGain…**,
and **Convert files…**, enabled when the selection has URIs and a music
root is configured. Invoking one is sugar over the shipped bridge: the
selection materializes through "Load as local files" into a local scratch
tab, and when that tab's asynchronous discovery finishes, the corresponding
dialog opens on it with all rows selected. The write pipeline, journaling,
and authority model are untouched — the mutation still happens on rows of a
persisted local list document, exactly as if the user had run the two steps
by hand.

Supporting decisions:

- The effective music root prefers the connected profile's local music root
  (ADR-0174) and falls back to the global settings folder (ADR-0112), so
  both configuration surfaces keep working.
- The bridge now resolves every URI through `mpd::resolve_below_music_root`
  — traversal, absolute, scheme-carrying, and root-escaping URIs count as
  misses and are reported in the status bar, never joined naively.
- Stored-playlist and committed-search tabs also gain "Load as local
  files"; like the sugar actions, these never talk to MPD and therefore do
  not require a ready connection.
- The menubar Edit-menu actions stay local-only: their handlers act on the
  current local tab, and Alt+Return in MPD context would be ambiguous while
  the materialized tab is still filling. The sugar lives in the context
  menus, as separate actions with their own object names.
- If a folder scan is already running, the materialization is refused with
  the existing message and no dialog follow-up is armed; a follow-up fires
  only for the discovery that targets the tab it created.

## Consequences

- Right-click → Edit tags on an MPD selection is now one action instead of
  three, with the materialized tab left visible as the record of where the
  edit happened.
- The mapping itself stays lexical and read-only (`docs/mpd-client.md`);
  what changed is that three named actions now perform the explicit
  materialization on the user's behalf. ADR-0058's rule that MPD rows never
  enter local mutation plans implicitly remains true — rows are mutated
  only after becoming local rows.
- Ratings, playback, and navigation keep their MPD-native paths; none of
  the list-organisation actions apply to server queues. Local audition of
  mapped files was considered and excluded.
- The deferred "opening a mapped server item as an explicit local source"
  bullet in `docs/open-decisions.md` is resolved by this ADR; the
  post-publication MPD database update was already an explicit action
  (ADR-0112 addendum).
