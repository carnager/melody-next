# ADR-0238: The Actions button opens a popover, and remembers

## Status

Accepted, 2026-09-29. Supersedes the menu of ADR-0186; its hidden-controls
state model stays.

## Context

ADR-0186 put the tagger's apply options in a footer menu. Two frictions
showed in use:

- A menu closes on every click. Turning on Rename *and* Move, choosing a
  layout and ticking a script meant opening it again for each, through
  nested submenus.
- Nothing was remembered. Every Properties window started with Rename and
  Move off, so tagging and filing an album meant setting them again each
  time -- and forgetting was easy, since they were out of sight.

## Decision

- **Actions opens a popover**: one small panel under the button with every
  choice in view -- Save tags; Rename files with its naming layout; Move
  files with its destination (the engine's, ADR-0237); links to manage
  them; the ReplayGain grouping with Scan now and its links; the scripts
  as checkboxes with the script editor link. Changes take effect as they
  are made; it closes on a click elsewhere or Esc. No OK or Cancel.
- **Choices are remembered** for the next window: Save tags, Rename and
  Move as last chosen there, the naming layout, and the move destination
  of each engine separately. Rename and Move come back on once a layout
  (and a destination) make them possible. A layout or destination that has
  gone falls back to the first, not to none.
- The footer summary stays the at-a-glance answer to what Apply will do,
  so a remembered Move is never a surprise.

The popover is built from the same hidden controls the menu drove, so
gating, apply semantics and the tests addressing those controls are
unchanged.
