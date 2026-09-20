# ADR-0198: One interaction path for server lists

## Status

Accepted, 2026-09-20. User clarification of ADR-0191 and ADR-0196.

## Decision

MPD Queue and named server-list tabs are equivalent track surfaces. Active
playback is state of a list, not a different kind of user interaction. One
track-menu builder and selection normalization handles both, including grouped
album-header selection. Shared actions resolve their source from that view;
none may silently read the MPD Queue selection when another list is targeted.

Play, Up Next, Last.fm, mapped local tools, navigation, removal/crop, ratings,
and transfers have the same ordering. Whole-list management belongs to the
list/tab surface, not a separate playlist-only tail in the track menu. Priority
is enabled only for the active list, resolved to current server occurrence IDs;
inactive lists cannot set priority on an unrelated materialized queue.

MPD protocol addressing still differs: the unnamed queue context and named
stored playlists use their existing commands. This is an adapter detail. It
does not justify separate track-menu code or an ambiguous “live queue” action.
Play (including keyboard and double-click activation), Remove, and Crop use
a shared controller API addressed to the originating list. Playing a named
list activates that context instead of appending its tracks to MPD Queue.
The playlist sidebar uses Play list or Up Next, and explicit Send to tab targets.
Its track activation plays within its owning list. The stock-MPD library
fallback labels the destination MPD Queue explicitly.

## Validation

Regression tests compare complete track-menu ordering for the same queue/list
contents and verify selection-aware navigation, ratings, and removal behavior.

Verified with the development build and the `bench-main-window`, `mpd-client`,
`mpd-session`, and `mpd-queue-model` CTest suites (all four passed). Formatting,
diff whitespace, and SPDX checks also pass.
