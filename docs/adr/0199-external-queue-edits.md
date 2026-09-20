# ADR-0199: External queue edits follow the active server list

## Status

Accepted, 2026-09-20. Corrects the remaining pre-ADR-0191 materialization-only behavior.

## Decision

Successful ordinary MPD queue edits publish the resulting normal track order
back to the active named stored playlist in one database transaction, then emit
`stored_playlist` so clients reread their render caches. This includes external
clear/add/play sequences. Temporary Up Next occurrences are excluded; inactive
lists and the displaced unnamed queue are untouched. The unnamed context keeps
ordinary MPD queue semantics. Persistence failures report that the live queue
changed but its stored list could not be saved.

Trackknife marks the playback list with an explicit “Active” tab suffix,
independent of which tab is selected for browsing. The marker applies to local
and server playback and remains in place while paused or stopped. Only playing
a different list changes the active list; selecting a tab only changes browsing.
Local active-list identity is separate from transient playback/decoder state.

## Validation

Daemon regressions cover external clear/add/play, switch-away-and-back
persistence, displaced-queue preservation, and exclusion of Up Next requests.
The UI regression checks that selecting another tab does not move the local
playback marker, stopping retains it, and explicitly activating another list
moves it.
