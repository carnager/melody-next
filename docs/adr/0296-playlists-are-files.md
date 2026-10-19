# ADR-0296: Saved playlists are the files in the playlist folder

## Status

Accepted, 2026-10-19. Changes where ADR-0233's saved lists live: the
engine's list store becomes the index of a folder of playlist files. The
`list.*` protocol, revisions, edits (ADR-0256), `list.play` and
continuations (ADR-0253) are unchanged.

## Context

- **Melody's saved playlists were only the engine's.** Another player could
  not read them; a folder of M3U files -- MPD's `playlist_directory`, a
  phone's sync, an old foobar2000 export -- could only be brought in one
  file at a time, as a copy (ADR-0128).
- **Someone moving from MPD wants to drop their playlists in a folder** and
  have them, with tracks named as MPD names them: relative to its
  `music_directory`, not to the playlist.
- **Two kinds of playlist -- Melody's and files -- would need explaining,
  and syncing two copies of each.** One of them has to be the truth; with
  other programs writing the files, it is the files.

## Decision

1. **Each engine has a playlist folder**: `Playlists` in its first music
   folder unless another is chosen (`playlists.set_folder`, Settings ›
   Engines, `melody-cli playlists folder`). The default is made when first
   needed, inside a music folder that holds something -- never on the disk
   beneath a share that is not mounted; a folder chosen is never made.
2. **Every `.m3u`, `.m3u8` and `.pls` there is a saved list**, named by its
   file. The engine reads the folder at start, every five seconds, and when
   asked (`playlists.sync`): a file new, changed or gone makes, changes or
   drops its list, announced with `list.changed`. A folder not there drops
   nothing.
3. **Every saved list is its file.** A change announced by any path -- a
   client's save, edit, rename or delete, a move followed (ADR-0237) -- is
   written to the file, whole, replacing it atomically; a rename renames
   it; a delete moves it to `.deleted/` in the folder, not away. Saved lists
   from before are written there once.
4. **Lines are read leniently.** A relative line is the file beside the
   playlist, else under each music folder (MPD's), never climbing out of
   one; a web address or Windows path is passed over and counted; a title
   not in UTF-8 is read as Latin-1; a path keeps its bytes.
5. **A file keeps its style.** Written again, it names its tracks as it did
   -- relative to the music folder, to the playlist, or whole. New files
   use the engine's style, relative to the music folder by default, so MPD
   reads what Melody writes.
6. **What only Melody keeps rides along.** `#MELODY-LIST:` names the list,
   so a file renamed by another program stays the same list; `#MELODY:`
   before a line keeps what a file cannot say -- a CUE track's segment, a
   subsong, a sidecar's ReplayGain. Other players skip both. A list's entry
   identities are kept across a re-read by matching paths in order.
7. **In the window, the library's Playlists are a source tab** beside it
   (ADR-0298): the saved playlists of the engine holding the library; one
   chosen opens as a tab, its menu opens, renames and deletes it, and tracks
   dropped on one are added to it.
8. **The engine keeps its index** -- the folder, the style, which file is
   which list -- in `playlists.json` in its state folder. Protocol level 19.

## Consequences

- MPD's playlists are Melody's by putting them in the folder, or choosing
  MPD's folder; Melody's are MPD's.
- A file edited elsewhere while a client has unsaved edits of its list is a
  conflict there, as any change by another client is (ADR-0256).
- Playlists stay one engine's: a playlist on the NAS engine is a file on
  the NAS.

## Verification

- `playlist-file`: MPD's, other players' and Melody's files read and written
  again alike; PLS; Latin-1 titles and byte names.
- `engine-playlist-folder`: saved, edited, renamed and deleted lists as
  files; an MPD file dropped in, edited and taken away; a restart without
  duplicates; another folder and style chosen; a folder gone drops nothing
  and is not made again.
- `cli`: `melody-cli playlists` says and chooses the folder.
- `bench-main-window`: Settings › Engines says the folder and sets the style;
  an MPD file dropped in the folder shows under Playlists and opens as a tab.
