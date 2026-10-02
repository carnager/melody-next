# ADR-0250: The widgets window is drawn as the Qt Quick window is

## Status

Accepted, 2026-10-02. Revises ADR-0247's "the desktop's style under the
desktop's colours" for the widgets window. Amended by ADR-0252: the Qt Quick
window is retired, and `TrackknifeStyle` is now the source of the measures
below rather than a copy of Theme.qml.

## Context

Trackknife has two windows over one workspace: the widgets window and the Qt
Quick window (ADR-0240). The Quick window draws every control itself from one
set of measures and colours (`src/quick/style/Theme.qml`): 28 px controls,
4 px corners, flat fills mixed from the palette, the accent on the accepting
button, underlined tabs, thin scroll bars, rows told apart by spacing rather
than a grid. The widgets window took the desktop's style -- or Fusion under
Trackknife's own palettes -- and looked like a different application:
bevelled 24 px buttons, boxed tabs, grid lines, full-strength selections, and
in the tagger a cramped frame around a different arrangement of the same
parts.

## Decision

- **One look.** The widgets window always uses `TrackknifeStyle`, a proxy
  over Fusion that draws buttons, fields, check boxes, radio buttons, combo
  boxes, spin boxes, tabs, headers, item-view rows, menus, scroll bars,
  sliders, progress bars and splitters with Theme.qml's measures and colour
  mixes. Its constants mirror Theme.qml and are kept in step with it by hand.
- **The scheme decides only colours.** *As the system* keeps the desktop's
  palette; Light and Dark keep Trackknife's (ADR-0247). The desktop's widget
  style is no longer used, as the Quick window never used it.
- **The accent** is on a checked button and on the button a window names as
  its primary (`QPushButton::isDefault`), faded when disabled -- not on any
  button that is a dialog's default for a moment because it has focus.
- **Rows**: a chosen row is a quiet tint (Theme.selection) with its text
  unchanged; a list whose check boxes say what is chosen shows no second
  selection, only an outline where the keyboard is.
- **Icons** are named as Quick names them, theme names with a style fallback
  (`edit-undo|sp:SP_ArrowBack`, `themedIcon`), so a desktop without an icon
  theme -- macOS -- still shows them.
- **Windows follow Quick's arrangement**, starting with the tag editor: a
  shaded header and footer across the window, the files in a shaded side pane
  with their folder above, the field sections beside them with 16 px around,
  Close then Apply at the right end with Apply in the accent.

## Consequences

- The two windows look alike on every desktop, and a fix to the look is made
  in two places that say the same thing, not two looks.
- A desktop's own widget style (Kvantum, Breeze) no longer reaches Trackknife.
- The main window, the queue and the remaining dialogs follow, window by
  window; until then they get the style's controls in their old arrangement.
