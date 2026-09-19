# ADR-0185: Settings screen and preference relocation

## Status

Accepted, 2026-09-19. Continues ADR-0183's compacting of the tag editor;
prepares the ADR-0184 cover-policy work.

## Context

The settings dialog held two rows (startup context, MPD music folder)
while every other preference lived wherever a widget happened to be: the
naming-layout and move-destination profile managers were non-modal
dialogs reachable only from inside the tag editor, and the set-once
ReplayGain scan preferences (sidecar-only, true peak) sat as checkboxes
in the editor's apply panel writing straight to QSettings. The direction
(Picard model): policy is configured once in Settings and the working
screens automate against it.

## Decision

- SettingsDialog becomes a paged screen (page list + stack): **General**
  (startup context, MPD music folder — keys unchanged), **Naming**,
  **ReplayGain**, and **Covers**.
- The profile managers move to the Naming page as a reusable
  `OutputProfilesManagerWidget` backed by the same injected
  `OutputProfileStore` (now declared alongside the widget); persistence
  is untouched. Selection is list-based; the tag-editor-only live path
  preview did not move. The widget emits `profilesChanged`, which the
  bench window fans out to every open tag editor's
  `reloadOutputProfiles()`, so the selector combos refresh immediately.
- The tag editor keeps its layout/destination selector combos; its two
  Edit buttons now emit `manageOutputProfilesRequested`, which opens
  Settings on the Naming page. The in-editor manager dialogs, their CRUD
  handlers, and the preview machinery are deleted.
- The ReplayGain preferences move to their Settings page; the editor
  reads the QSettings keys (`replaygain/sidecar-only`,
  `replaygain/true-peak`) at scan/plan time. The apply tab loses two
  more rows.
- The **Covers** page records the ADR-0184 policy keys (`artwork/embed`,
  `artwork/write-folder-image`, `artwork/folder-image-name`,
  `artwork/fetch-source`) as UI only; enforcement lands with the cover
  workflow.

## Consequences

- Preferences have one home; the tag editor's Apply & Scripts tab is
  down to the per-session controls (Save tags, Rename, Move, scan
  trigger, scripts).
- Simple values persist on Save; profile edits persist immediately
  through the store — the dialog's Save/Cancel does not roll back
  profile CRUD, matching the previous managers' behavior.
- SettingsDialog stays constructable without a store (the Naming page
  degrades to a note), keeping standalone tests trivial.
