# ADR-0205: Task-focused workspace command discovery

## Status

Accepted, 2026-09-21.

## Decision

Workspace → Commands opens a searchable palette with Ctrl+Shift+P. Ctrl+K keeps
its existing Connect to MPD binding. Settings → Shortcuts owns shortcut editing,
validation, and persistence; the palette does not provide another editor.

The workspace supplies an explicit inventory of tasks: opening music, managing
lists, searching, navigating playback, tagging, conversion, dynamic playlists,
connections, settings, and workspace backup/restore. Do not enumerate all
QActions recursively: output names, numeric ratings, modes, and column labels
are parameter choices and make poor standalone commands.

Commands reuse the existing actions and their current enabled/visible state.
Unavailable and hidden commands are omitted, including from search results,
and appear automatically when available. The inventory excludes context-menu
actions whose selection or
availability is initialized only when their menu opens. Main actions retain
their existing authority checks and confirmation flows.

Search matches all entered words against names, shortcuts, and stable command
IDs. Up/Down navigates while search retains focus; Enter runs and Escape closes.
Shortcuts occupy a right-aligned column; long command labels are elided to
preserve separation from their shortcuts.
Action references are guarded against destruction, and state changes preserve
the selected command. Opening the palette again raises the existing window.

## Verification

Workspace tests cover settings dispatch, single-instance reuse, the existing
connection shortcut, filtering out device/rating choices, multiple search
words, keyboard activation, hidden/disabled actions, deduplication, and action
destruction while the palette is open.
