# ADR-0201: Up Next panel presentation and motion

## Status

Accepted, 2026-09-20, following the request to make Up Next feel like the normal
queues and animate its appearance.

## Decision

Keep the existing shared flat track-view engine and authority-specific request
models. Use the normal row height, alternating rows, full-row selection, and
artist/title/length columns. Replace the large button row with a compact icon
toolbar and a track context menu sharing its actions. Keep the return destination
in a footer and show a short empty-queue hint. Extended selection supports Ctrl/Shift selection, batch removal, one-step
movement, and dragging a selection in order. Each batch retains occurrence IDs
and is one Undo step. Melody advertises `melody_upnext_edit` for atomic,
revision-checked complete pending-order edits; older servers retain single-row
editing, while multi-row mutation controls remain disabled.

Up Next is a resizable right-side panel with an explicit close button. It no
longer floats or docks on the left. A 180 ms eased width reveal clips stable-size
contents, avoiding repeated track-layout reflow. Reversing the toggle midway
starts from the current width. Remember the expanded width and desired visibility,
never the transient collapsed width. Startup restoration is immediate.

Settings → General offers “Animate panel opening and closing”, enabled by
default and committed only on Save. Disabling it makes subsequent toggles
immediate. This implementation applies to Up Next; ordinary tab switching stays
immediate.

## Validation

UI tests cover interrupted/reversed transitions, width restoration, immediate
toggling, settings Save/Cancel, and existing request editing/playback/persistence.

The transport volume icon toggles mute via the existing authority-owned volume
control. It remembers the previous nonzero level per server profile/output and
separately for local playback. Zero volume shows the muted icon; changing the
slider away from zero unmutes. Unsupported/disconnected volume disables both.
