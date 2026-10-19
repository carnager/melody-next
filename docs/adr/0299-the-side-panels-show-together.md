# ADR-0299: The side panels show together

## Status

Accepted, 2026-10-19. Changes how the side panels of ADR-0291 and ADR-0292
are shown; Panels (ADR-0294) becomes the one switch.

## Context

- **A plugin's panel had to be found** under Playback, after Lyrics, once
  the plugin was enabled -- enabling it was not enough to see it.
- **Each panel had a ×** and a toggle under Playback: two switches per
  panel, and a third, Panels, for them all.
- **Right-click on the window's bars** gave Qt's own list of every dock and
  toolbar: the side panels greyed -- not Qt's to close -- and the window's
  internal bars offered to hide. On macOS a friend took it for the way to
  show a panel, and could not.

## Decision

1. **A panel plugin enabled is a panel**: beside Lyrics, as a tab, with the
   others. Disabled, it is gone. There is no switch of its own.
2. **Panels -- the header's button, Ctrl+Shift+L -- shows or hides the side
   panels together**, remembered (`panels/shown`; Lyrics' own setting before
   there was one). Shown again, each asks about what plays.
3. **No ×, no Playback entries, no right-click menu** of Qt's.

## Verification

- `bench-main-window`: Panels shows and hides Lyrics, remembered, with
  Ctrl+Shift+L; an enabled plugin's panel is shown with them and asked;
  nothing of the panels under Playback, no ×, no menu of Qt's.
