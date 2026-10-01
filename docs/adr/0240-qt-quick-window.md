# ADR-0240: The Qt Quick window is a second view of one workspace, in Fusion everywhere

## Status

Proposal, 2026-09-30, on branch `qml-mockup`. Builds on ADR-0220 (one
engine; the UI asks, the core answers). Its system-palette colours are the default
since ADR-0247, beside Trackknife's own light and dark schemes.

## Context

Trackknife's window is Qt Widgets. Its look comes from each system's widget
style: Kvantum on the developer's desktop, a different one on macOS and on
Windows. The experiment on `qml-mockup` asks whether a Qt Quick window gives
one look on every system.

A first Qt Quick window was a redesign with reduced features. That is not
wanted. What is wanted is every menu, dialog and settings screen working as
the widgets window's do, with freedom in how it looks where a look is better.
The album view of that first window is one such case, and it is kept
(`queue.jpeg`).

## Decision

**One workspace, two windows.** The widgets window's logic moves into a
`Workspace` (`src/workspace`, no Qt Widgets) that both windows draw through a
`WorkspaceView`. Rules both windows show, such as album grouping, cell
contents, column fitting and the texts of tabs, modes and Up Next, are pure
functions in that library. A behaviour is written once. The Qt Quick window
(`src/quick`) only draws and takes input.

**Full depth, not a reduced app.** Every menu entry, context menu, dialog and
settings screen of `docs/quick-port-parity.md` is ported with its whole
behaviour. Until one is, its entry says so. Widgets dialogs are not reused
in the Qt Quick window: they would carry the system's widget style, the
inconsistency this window exists to remove.

**Fusion on every system.** The Qt Quick window pins the Fusion style (unless
`QT_QUICK_CONTROLS_STYLE` says otherwise). Fusion is stable, complete and
drawn from the palette, so the controls look and behave the same on Linux,
macOS and Windows. The colours are the system's palette, so light and dark
follow the desktop. Trackknife's own parts, the header, tab bar, album list,
sliders, status modes and Up Next, are drawn by the window itself with the
palette mixes the widgets window uses.

A built-in Trackknife palette, identical on every system, is a possible
later setting.

## Consequences

- The desktop's own Qt Quick style (KDE's `org.kde.desktop`, through
  Kvantum), chosen first, is dropped. It does not exist on macOS or Windows.
- Dialogs take longer to arrive in the Qt Quick window, because each is
  rebuilt rather than reused. Their logic moves into shared controllers on
  the way, which the widgets window uses too.

## Addendum: sessions behind dialogs

A dialog's logic moves into a session in `src/workspace`: a `QObject` with
no widgets that holds its state, models, selections, texts and enablement,
runs its work off the UI thread, and says what changed. Each window's dialog
is a view over it: the widgets dialog keeps its controls and object names,
so its tests still drive it; the Qt Quick dialog is drawn from the same
getters and calls the same methods. Where a view needs a model, it shares
the session's model and selection model rather than keeping its own.

The first are the tag editor's: `TaggerSession` (the editor),
`ArtworkSession` (its covers, and the editor's artwork itself),
`IdentifySession` and `TrackMatchSession` (MusicBrainz). `Workspace`
hands a tag editor its services (`taggerServices`), so both windows open
it the same way.
