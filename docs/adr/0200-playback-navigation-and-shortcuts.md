# ADR-0200: Playback navigation and keyboard settings

## Status

Accepted, 2026-09-20, following the user's requests for cursor-follow,
jump-to-playing, and a sensible editable shortcut set.

## Decision

Follow playback is opt-in and persisted. A track change selects and centers its
row only when the playback list is visible; it does not pull users away from
another tab or repeatedly override manual selections within the same track.
Jump to playing selects the playing list in the current authority, loading a
closed server list asynchronously if needed. Up Next requests reveal their
panel instead of using a base-list position. Neither action starts playback.

Settings → Shortcuts stages primary application bindings with Save/Cancel and
Restore defaults. Stable action object names are persistence keys; defaults
are captured before applying overrides. Empty values disable bindings. Duplicate
and chord-prefix conflicts are rejected before any change is applied. Native
text/list navigation retains its context. No desktop-wide shortcut registration
or Default-tab routing is introduced.

## Validation

UI regressions cover follow without tab stealing or timer-driven reselection,
manual jump with following disabled, sticky Active state, shortcut conflicts,
Save, Cancel, and binding restoration in a new window.

The development build and the complete `bench-main-window`, `mpris-service`,
and `lastfm-service` CTest suites pass. Whitespace and SPDX checks pass.
