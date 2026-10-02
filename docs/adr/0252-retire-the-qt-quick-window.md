# ADR-0252: The Qt Quick window is retired

## Status

Accepted, 2026-10-02. Supersedes ADR-0240. Amends ADR-0250: the look's
measures and colours now live in `TrackknifeStyle`, not in a copy of
`Theme.qml`.

## Context

ADR-0240 added a second window, `trackknife-quick`, in Qt Quick over the same
workspace as the widgets window, as a preview of a possible successor. Its
look was liked; ADR-0250 then drew the widgets window the same way -- the
style, the tag editor, the album view, the sources, the transport -- on top
of the widgets window's full function. With both looking alike, the second
window was only cost: every feature's interface built twice, and a parity
document to keep. A phone was the one reason left to keep Qt Quick, and the
phone already has its own client, the native Android app in `android/`.

## Decision

- **Remove the Qt Quick window** from main: `src/quick` (QML, its style
  plugin, `QuickWorkspace` and its adapters, image providers), the static
  mockup in `src/quickmock`, the `TRACKKNIFE_BUILD_QUICK` and
  `TRACKKNIFE_BUILD_QUICK_MOCKUP` options, the Quick-only keyed-rows test,
  its desktop entry and its packaging, and `docs/quick-port-parity.md`.
  The package no longer installs `trackknife-quick` nor needs
  `qt6-declarative`.
- **Keep the workspace whole.** Every `src/workspace` file also serves the
  widgets window; none is removed.
- **Archive, not a branch to maintain.** The last main with the window is the
  tag `quick-ui-final`; anything from it is one `git checkout quick-ui-final
  -- <path>` away.

## Consequences

- One desktop interface, the widgets window, drawn in Trackknife's own style
  (ADR-0250); `TrackknifeStyle` is the single source of its measures.
- Comments and documents describe one window; the history of the Quick
  window stays in its ADRs and the tag.
