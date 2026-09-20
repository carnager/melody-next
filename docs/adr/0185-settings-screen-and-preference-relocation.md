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

## Naming presentation update (2026-09-20)

Naming layouts and move destinations now occupy separate tabs within Naming.
Compact preset dropdowns replace the persistent selected lists, leaving the
full content width for labelled expression and destination editors. Save layout
and Save destination retain the same immediate store-backed persistence.
Settings navigation uses a muted selection fill and a narrow accent marker.

## Playback preferences update (2026-09-20)

Settings adds a Playback page for local buffer presets/custom durations and
ReplayGain preamps with/without gain metadata. General adds background track
notifications. Existing preference keys and playback-menu shortcuts remain
shared; Settings saves only on Save, and Cancel changes neither live playback
nor persisted preferences. Saving applies preamps and notification preferences
to the running workspace; buffer changes follow the existing next-track boundary.
The page states that MPD playback uses server settings. Custom start thresholds
cannot exceed capacity, and preamps retain the audio core's existing limits.

## Library folders update (2026-09-20)

Library in Settings hosts the existing asynchronous indexed-folder manager.
The sidebar Folders shortcut opens this page, reusing an already open Settings
screen; standalone library panels retain the same manager in a small dialog.
Folder additions and removals persist immediately, as the page explains, and
removal leaves the underlying files untouched. Opening Settings or adding a
folder never initiates a filesystem scan: the sidebar Refresh action remains
the explicit scan trigger. Folder paths retain raw-byte identity.

## Connection profiles update (2026-09-20)

Connections in Settings manages saved MPD/Melody profiles through asynchronous
profile persistence: name, host/socket, port, local music-folder mapping, and
the single startup connection. Save profile and Remove persist immediately;
failures leave both the committed profile set and the editable form intact.
Settings does not connect or disconnect sessions. Existing Connect remains the
quick connection action and accepts session-only passwords. Unedited raw folder
paths survive profile edits exactly. The existing fallback music-folder setting
moves from General to Connections and still follows Settings Save/Cancel.

## Metadata services and consistency update (2026-09-20)

Metadata services exposes the existing AcoustID client-key preference with
masked entry and an explicit Show control. Save trims and stores the key,
Cancel preserves it, and clearing then saving disables key-backed lookup.
MusicBrainz text search needs no credentials. The page explains local storage
and explicit fingerprint lookups; no network call occurs while editing settings.
Missing-key errors direct users to this page rather than a settings key name.

Custom-buffer and ReplayGain-preamp shortcuts now open the existing Playback
settings controls; duplicate dialogs are removed. Settings pages scroll and
forms wrap on smaller windows. Page-specific footer text distinguishes ordinary
Save/Cancel preferences from immediate profile and indexed-folder operations.
