# ADR-0186: The Actions menu replaces the Apply & Scripts tab

## Status

Accepted, 2026-09-19. Supersedes the "third sections tab" part of
ADR-0183; completes the relocation ADR-0185 started.

## Context

After ADR-0185 moved profile management and the ReplayGain preferences
into Settings, the Apply & Scripts tab held only per-session choices:
three checkboxes, two preset selectors, a scan trigger, and the scripts
list. A whole tab for that is ceremony — the user asked for a single
button that chooses which actions run, picks their presets, and links to
the matching settings.

## Decision

The tab is gone; the sections widget is back to Fields and Artwork. The
footer gains an **Actions** menu button next to the apply summary:

- **Save tags / Rename files / Move files** as checkable entries, with
  **Naming layout** and **Move destination** preset submenus and
  Manage… entries opening Settings on the Naming page.
- A **ReplayGain** submenu: Scan selection now, the grouping presets
  (the expression grouping prompts for its tkfmt source), Loudness
  sources…, and ReplayGain settings….
- A **Scripts** submenu: the saved scripts as checkable run-on-apply
  entries plus Open script editor….

The menu is rebuilt on open and proxies the previous panel's controls,
which stay alive hidden as the state model — apply semantics, enablement
gating, and persistence behavior are byte-for-byte unchanged, and the
footer summary (ADR-0183) remains the at-a-glance answer to "what will
Apply do". The editor's settings links generalize to one
`openSettingsRequested(page)` signal.

## Consequences

- The editor surface is now exactly: files (sidebar-hosted), the field
  table, artwork, one Actions button, and Apply — the Picard shape the
  user asked for.
- Tests keep addressing the hidden controls by object name; menu
  entries have their own `action-metadata-*` names.
- If the hidden-panel state model ever becomes a maintenance burden, the
  follow-up is extracting a plain state object — not resurrecting the
  tab.
