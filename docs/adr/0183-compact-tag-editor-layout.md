# ADR-0183: Compact tag editor layout

## Status

Accepted, 2026-09-19. The Apply & Scripts tab introduced here was
superseded by the footer Actions menu (ADR-0186); the header, footer
summary, and sidebar-hosted file list stand.

## Context

The tag editor is embedded as a workspace tab, so it can never exceed the
main window — on a 1366×768 laptop that is roughly 1350×640. Inside that
budget the previous layout spent a permanent 260–380 px splitter pane on
apply options whose controls are mostly set-once or dormant (Rename/Move
are disabled until a layout/destination is configured; the ReplayGain
sidecar and true-peak toggles are persisted preferences; the scripts list
is empty until scripts exist), plus three stacked header rows and a
nine-button tool row, before the field table received a pixel. The
field-set save/delete feature was reachable from nowhere: its only
triggers were hidden buttons never added to any layout.

Two verified facts make the reorganization safe: Apply builds its write
plan purely from checkbox state, never from visibility, and the offscreen
tests locate widgets by object name, which ignores visibility.

## Decision

- The apply/scripts side panel becomes the third sections tab, **Apply &
  Scripts**, next to Fields and Artwork, wrapped in a scroll area so its
  height stops contributing to the dialog minimum. The horizontal content
  splitter is gone; the field table always gets the full width. The
  `workspace/metadata-properties-content-splitter-v1` layout key retires;
  geometry and the vertical files/sections splitter keep persisting.
- Because the options are now one tab away, the footer gains an
  at-a-glance plan summary (`Apply: tags · covers · rename · move`)
  rebuilt whenever Apply enablement recomputes, and empty when nothing is
  staged.
- The selection summary and the ADR-0152 technical line share one header
  row; the technical text clips with its full content in the tooltip
  instead of wrapping into extra rows.
- Undo/Redo/Discard become icon buttons with tooltips and accessible
  names; all object names are unchanged.
- The field-set save/delete entries move into the More menu, replacing
  the unreachable hidden buttons.

## Addendum: sidebar-hosted file list

Even full-width, the in-editor file list was a wide flat band spending a
third of the height on a path column — the wrong shape for a path list.
While a tag editor tab is active, the local sources sidebar gains a
temporary **Files** page: a mirror view sharing the editor's model and
selection model (nothing reparents, so the dialog's object tree and every
dialog-scoped lookup stay intact), with a breadcrumb of the selection's
common folder above rows rendered relative to it — plain filenames for
the ordinary one-album edit. The in-dialog copy hides while mirrored and
returns when the editor's tab is left or closed; the temporary page is
never persisted as the default sidebar view. Standalone dialogs keep the
stacked layout.

## Consequences

- On small screens the editor is a files list over a full-width field
  table — the actual editing surface — with everything else one tab away
  and the footer stating what Apply will do.
- Checkbox gating and enablement logic run regardless of which tab is
  visible; Apply semantics are unchanged.
- Follow-up work (settings-screen relocation of naming profiles and
  ReplayGain preferences, and a policy-driven cover workflow) is recorded
  in the agent task list rather than decided here.
