# Roadmap

Updated on 2026-09-27, through ADR-0236.

This page lists the work still to do. The [feature matrix](feature-matrix.md)
records what is implemented; [the unified-engine plan](unified-engine.md) says
how far each phase of the engine has got. An item here is not a release
promise.

**Proposal:** Keep the order below as a guide. Bugs in an existing workflow
come before adding more features to it.

## Where things are

The engine is in place (ADR-0220). `melodyd` owns the library, the lists and
playback; Trackknife, the Android app and `melody-cli` are its clients; sound
comes out of output agents. Trackknife holds a connection to every engine you
list, each with its own library and lists (ADR-0234). The MPD client is gone
(ADR-0224). File work -- tagging, ReplayGain, conversion, moves -- is still
done by Trackknife, on files it can reach, including an engine's files over a
mount.

## 1. Engine and clients

- [ ] **Online radio** (asked for): internet streams as something to play.
  Needs an ADR first -- where stations live (the engine, like lists), playing
  a stream with no end, stream titles (ICY), what is scrobbled, and the phone.
- [ ] The engine watching its own music folders. `melody-watch` (ADR-0232)
  tells an engine about changes on a NAS; an engine noticing them itself is
  still open.
- [ ] Trackknife's own copy of the lists goes (ADR-0233, step 3d), once engine
  lists have been in use for a while.
- [ ] A terminal client (`melody-tui`). `melody-cli` covers scripting.
- [ ] Phase 5, the MPD bridge: the engine answering MPD clients. Low
  priority; nothing waits for it.
- [ ] Fuzz targets for what engines read from the network and from files:
  protocol v1, `tkq-1`, `tkfmt-1`, CUE sheets.

## 2. Lists and library views

Lists can be found, sorted, reversed, deduplicated, and edited with undo,
including a move between tabs. M3U8 import and export work. Searches can be
kept in tabs, saved, and run against any engine's library.

- [ ] Custom `tkfmt-1` columns and grouping in track views, and custom
  library tree views.
- [ ] Autoplaylists that update when the library changes. Saved searches run
  when opened; a result tab keeps a snapshot.
- [ ] CUE, chapter and subsong titles indexed as tracks of their own.
- [ ] An album-cover grid in Trackknife (the phone has one).
- [ ] Export over an existing playlist file with a reviewed replacement, and
  playlist formats beyond M3U8.

Removing an entry from a list must never delete its file.

## 3. Album conversion

The converter carries one cover, transfers text tags, and can mirror source
folders or name output with `tkfmt-1`. It resamples, limits sample rate
downwards only, sets bit depth and channels, and can apply Track or Album
ReplayGain permanently.

- [ ] Carry every embedded image, not only one.
- [ ] Say which selected files have no usable cover.
- [ ] Scan converted output for fresh ReplayGain values.
- [ ] Decide how grouped chapter containers and merge-all output carry
  metadata and boundaries. CUE tracks already split into exact files.
- [ ] A general DSP graph, if a real use case beyond mastering needs one.

New options must keep the converter's output checks and collision checks.

## 4. Tagging and artwork across formats

Text editing supports FLAC, WavPack, MP3, Vorbis, Opus and MP4/M4A; artwork
editing supports FLAC, MP3 and MP4/M4A.

- [ ] Identifying many albums at once: group, look up in the background,
  review only what needs it, apply once, with renaming, moving and
  ReplayGain as options (ADR-0261).
- [ ] Artwork editing in more containers, Ogg first.
- [ ] An ALAC fixture for MP4 text-writing tests.
- [ ] Deleting external cover files through a reviewed file operation.
- [ ] More text writers, each backed by real-file tests showing that audio,
  unknown tags and container data survive the rewrite.

Playing a format is not evidence that it can be edited safely.

## 5. ReplayGain

Track and album scanning, multi-disc grouping, true peak, retry, CSV export
and per-value provenance work; results go into tags, CUE sheets or `.tkmeta`
sidecars. Output-gain editing for Opus and scanning converted output remain
optional.

## 6. Listening history and ratings

Play counts, last played, resume, ratings on a 0–10 scale, album shuffle and
history queries all live in the engine that plays the music, and scrobbling
happens there too.

- [ ] Optional, opt-in writing of ratings into file tags (POPM, `RATING`)
  through the previewed metadata workflow.
- [ ] Calendar-relative time operators in queries.
- [ ] Optional autoplay when a list ends, choosing related tracks from the
  library. Online similarity needs an explicit opt-in, caching, and marking
  which tracks were added automatically.

## 7. Collection maintenance

- [ ] Integrity scans that tell decoding failures from checksum failures.
- [ ] Duplicate audio found by content, not by path or tags.
- [ ] Relinking missing files.
- [ ] Album completeness checks that show the evidence for missing tracks.

Finding two copies of a recording should help someone review them. It must
not imply that one is safe to delete.

## 8. Desktop integration

MPRIS and media keys control the engine that plays.

- [ ] Artwork in desktop notifications.

## Other recorded follow-ups

These are requirements or proposals on record, not commitments for the next
release.

- **Workspace:** a command palette, and job, diagnostic and queue-inspector
  panels.
- **Metadata and paths:** further versioned sanitization policies, richer
  typed matching, `TOTALTRACKS` when numbering, and moving companion files with
  reviewed cleanup of empty folders.
- **File undo:** across filesystems, for changed artifacts, and the artwork
  undo chain where rename-exchange is missing.
- **Infrastructure:** secure credential storage, backup and restore, larger
  library, network and device test runs.
- **Later:** release packaging, plugins, CD ripping.

## Keeping this page

Keep current support and its limits in the feature matrix. Take an item off
this page when it is done rather than leaving it ticked. ADRs and the dated
milestone notes remain the history of how decisions were made.
