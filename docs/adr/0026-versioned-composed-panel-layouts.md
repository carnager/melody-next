# ADR-0026: Versioned composed panel layouts

- Status: replaced by ADR-0292 (2026-10-17): one layout, side panels as tabs
- Date: 2026-08-29
- Owners: Trackknife project
- Amends: ADR-0003's native-dock-only workspace composition for Trackbench

## Context

Trackbench will add metadata, selection, job, ReplayGain, and conversion
surfaces over several milestones. A fixed sidebar plus one central tab widget
would force those tools into modal windows or a growing set of hard-coded
docks. The desired workstation experience is closer to the flexibility users
value in foobar2000's Columns UI: panels can participate in nested splits and
tab stacks, while a strong default remains immediately useful.

This is structural inspiration, not a Columns UI visual, component, layout-file,
or plugin compatibility target. Trackbench remains a native Qt Widgets
application and its panels keep typed internal boundaries. This ADR governs
outer panel placement only; ADR-0027 separately governs the presentation of
the queue/list rows inside the Track Lists panel.

## Decision

- Trackbench workspace composition is a versioned declarative tree. Version 1
  has three node kinds: a registered panel instance, a horizontal/vertical
  splitter with positive weights, and a tab stack with one active child.
- Panel instance IDs are stable and unique within a layout. The renderer obtains
  their existing widgets from a registry; the layout owns placement and sizing,
  never track, playback, file, or job state.
- The shipped default remains the product: Folders on the left and Track Lists
  on the right. Layout editing is an explicit mode layered over it. Users may
  reorient, stack, or reorder the panels and reset to the default.
- Layout JSON is validated before rendering with bounded depth/node counts,
  known unique panel IDs, complete registered-panel placement, valid child
  counts, and valid weights/selections. Invalid current-version data falls back
  visibly to the default.
- A newer/unknown persisted layout is not overwritten merely because an older
  application displayed the fallback. Only an explicit layout edit or reset
  replaces it.
- Split positions and selected panel tabs persist as part of the same versioned
  tree. Future schema changes require an explicit migration or a new version;
  existing schema behavior is not silently reinterpreted.
- New functional panels arrive with their owning milestones. Empty placeholder
  surfaces are not added merely to demonstrate layout flexibility.

## Alternatives considered

### Continue with QMainWindow docks only

Native docks are useful for auxiliary tools but do not provide a clean,
serializable nested composition model. The resulting behavior also varies by
window manager and is difficult to validate independently of live widgets.

### Clone Columns UI or its component model

Rejected. Trackbench does not target foobar2000 configuration or component
compatibility, and importing that API would bind Linux-native UI architecture
to external historical behavior the product does not need.

### Build the complete future panel catalog now

Rejected. It would create hollow UI and stabilize boundaries before metadata,
jobs, ReplayGain, and conversion have real application services. The layout
tree begins with the two implemented panels and expands alongside proven
features.

## Consequences

- Trackbench's center workspace is hosted by the composed renderer rather than
  treating Folders as a special QDockWidget.
- Layout serialization and validation become shared Widgets infrastructure,
  available to Trackknife only if a later product decision adopts it there.
- Panel widgets must tolerate reparenting without restarting asynchronous work
  or losing selection/model state.
- Layout editing and restoration join the UI responsiveness, accessibility,
  reset, and persistence acceptance surface.

## Validation

- Unit coverage round-trips split/tab layouts and rejects unknown, duplicate,
  incomplete, malformed, excessive, and newer-version trees.
- An offscreen Trackbench regression changes arrangement and order, persists a
  tab stack across restart, resets the shipped default, and proves a newer
  layout survives fallback without being overwritten.
- Existing playback, discovery, list, artwork, persistence, and performance
  tests remain unchanged because panel composition does not own their state.

## Revisit when

- the first metadata and job panels test nested compositions beyond two panel
  instances;
- users need reusable named layouts, import/export, or multiple instances of
  one panel type;
- runtime panel/plugin registration has enough built-in consumers to justify a
  stable extension boundary.
