# ADR-0262: One Apply for both windows, ReplayGain an Apply action

## Status

Accepted, 2026-10-04. Amends ADR-0238 (the Actions popover) and ADR-0261
(Identify albums…: its Apply page, and ReplayGain as a second step).

## Context

ADR-0261 gave Identify albums… an Apply page of its own: a checkbox per
album, Rename and Move each with a preset, Move into the library folder
each album is in, and ReplayGain scanned after the write. In use:

- It was a second Apply beside the tagger's Actions popover, with other
  choices and other words for the same things, and without the move
  destinations the popover has: a saved destination ("MPD Library") could
  not be chosen at all.
- Neither could a folder that is not a saved destination, in either
  window.
- ReplayGain in the tagger was a separate Scan now, staging gains to be
  written by a later Apply; in the batch, a second write after the first.
- The batch took clicks a lookup queue would not: a grouping page before
  anything was looked up, Review to be asked for, an Apply page to reach.

## Decision

- **One Apply, the tagger's.** Both windows show the same Actions popover
  over the same tagger session -- the batch window is a view onto that
  tagger -- so its choices are one state: Save tags, Rename files and Move
  files by one naming layout into one destination, ReplayGain, scripts.
  The naming layout's folder part is made inside the destination; its
  file-name part names the files when renaming (as before).
- **ReplayGain is an Apply action**: `[x] ReplayGain [grouping ▾] [x] Skip
  existing`, replacing Scan now. On Apply the files are measured first, the
  gains staged into the draft, and everything written in one write: tagging
  and moving never change the audio, so measuring before the write is
  measuring the files written, and a moved file gets its gain where it
  goes. *Skip existing* (on by default) leaves alone a group whose files
  all have the gain -- album gain, or track gain for *Track gains only*.
  Remembered with the other choices.
- **Choose folder…** in the destination list opens the engine's folder
  browser; the folder chosen is used and offered again next time, without
  becoming a saved destination. **The library folder each is in** is a
  choice there too.
- **Identify albums… streamlined:** looking up starts when it opens, the
  albums editable meanwhile; the review opens by itself when an album needs
  a person; staged albums have their checkbox in the list; its bottom bar
  holds Actions… and Write. Its Apply page goes.

## Consequences

- One component and one set of choices; what the batch writes is what the
  tagger would.
- One write and one journal per Apply, gains included.
- Scan now's review of measured gains as drafts before Apply is gone from
  the popover; the measuring is the same pipeline.

## Progress

- ReplayGain as an Apply action in the tagger (2026-10-04): the popover's
  ReplayGain checkbox, grouping and Skip existing; Apply measures the
  files lacking gain, stages the gains and writes once
  (`measureBeforeWrite`, `itemsNeedingGain`).
