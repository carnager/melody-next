# ADR-0194: One file view per tag editor

Status: Accepted (2026-09-20)

## Context

The Files sidebar previously mirrored a hidden editor table, sharing its model
and selection but maintaining a separate renderer. This duplicated presentation
and caused checkbox styling to reach only the hidden view.

## Decision

**Trackknife decision:** each editor owns its metadata model and selection model
and creates exactly one file-list widget. The workspace reparents that widget
into the Files sidebar while its editor is active and returns it to the editor
when leaving the tab. Standalone editors display the same widget locally.
The editor deletes its file view on destruction even while hosted. A guarded
pointer also handles the sidebar being destroyed first during window teardown.
Relative filenames are a delegate presentation property reset on return.

## Verification

UI regressions cover checkbox scope, individual and bulk edits, one file view
per editor, independent selections across two editor tabs, returning the view,
and destruction of hosted views when editors close.
