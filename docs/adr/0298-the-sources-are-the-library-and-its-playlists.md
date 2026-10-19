# ADR-0298: The sources are the library and its playlists

## Status

Accepted, 2026-10-19. Retires the This computer folder browser and its
bookmarks (ADR-0130, ADR-0286's "This computer" tab) and the lists as a pane
beside the tracks (ADR-0233); moves ADR-0296's playlists into a tab of
their own.

## Context

- **The folder browser was a bare one** -- a tree, bookmarks, Add to list --
  next to file managers that do browsing far better, and that drop files on
  Melody as well as it took them from its own tree.
- **Playlists under the library** (ADR-0296) took room from the library
  when not wanted, and room from the playlists when the library was.
- **Lists as a pane instead of a tab bar** was a second way to see the
  lists, kept up as everything else changed, and hardly used.
- **Workspace › Track list layout listed two shipped views** -- of four --
  and none of the user's (ADR-0282), which only the column header's menu
  offered.

## Decision

1. **The source tabs are the library and Playlists**: the library -- this
   computer's, or the tab of the engine holding it -- and, last, its
   Playlists: that engine's saved playlists. With no library to show, the
   panel says its engine is not connected.
2. **Files from this computer come in as before, without a browser here**:
   File › Open files…, Open folder…, dropped from a file manager, given on
   the command line. The bookmarks go; a source of Folders kept from before
   opens the library.
3. **Lists are tabs**: Workspace › Lists in a side panel and Settings ›
   General › Show lists as go.
4. **Workspace › Track list layout lists every list view**, shipped and the
   user's own, the one shown checked, made again when they change.

## Verification

- `bench-main-window`: Library first and Playlists last, remembered when
  chosen; the remote's library tab before Playlists; the library's
  playlists, opened and dropped on; Ctrl+L from Playlists to the library's
  search; a folder dropped from a file manager onto a list; the track list
  layout menu with every view, one of the user's made and chosen.
- `local-library`: the library opens first.
