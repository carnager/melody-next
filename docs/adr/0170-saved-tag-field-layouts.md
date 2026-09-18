# ADR-0170: Saved tag-field layouts

## Status

Accepted.

## Context

The M5 tag grid can expose arbitrary fields, but large selections make a single
ever-growing field list inefficient. The product specification requires saved
field layouts and task presets without allowing presentation choices to omit
staged writes.

## Decision

The Fields toolbar gains a saved-layout selector plus **Save layout…** and
**Remove layout**. Saving captures the selected field rows, or all currently
visible rows when none are selected. A layout stores an ordered list of
canonical field names. Choosing it immediately filters the Fields presentation;
the active layout also supplies preferred field order before the next metadata
selection is projected.

Layouts use a bounded schema-1 JSON envelope in the existing serialized
workspace UI-state store: at most 64 layouts and 256 unique non-empty fields per
layout. The active layout is persisted. Malformed, unknown-version, empty, and
over-limit entries are ignored conservatively.

Layouts never alter the staged selection or sparse patch set. Apply, scripts,
MusicBrainz, ReplayGain, undo, and conflict detection continue to operate on
the complete selection, including fields hidden by a layout or text filter.
Adding a field still clears presentation filters so the new draft is visible.

## Consequences

Users can keep focused tagging presets without maintaining a fixed global
field vocabulary. Exact-native fields remain addressable by their canonical UI
identity. A future layout schema may add widths or richer task configuration,
but must not reinterpret schema 1.

## Evidence

The offscreen workspace test loads a persisted layout before projection,
verifies preferred ordering and visibility, switches back to all fields, and
checks the active choice was saved through the asynchronous persistence seam.
