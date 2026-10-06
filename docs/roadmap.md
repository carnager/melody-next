# Roadmap

Updated on 2026-10-05, through ADR-0269.

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
list, each with its own library and lists (ADR-0234), and keeps no lists of
its own (ADR-0259). The MPD client is gone (ADR-0224). File work -- tagging,
ReplayGain, conversion, moves -- is done by the engine whose files they are,
on its machine (ADR-0237); what a write replaced can be undone (ADRs 0263,
0266). What shows a library follows its changes as they happen (ADR-0268).

## Toward 1.0

1.0 is cut when the conditions in [MILESTONES.md](../MILESTONES.md#toward-10)
hold. What remains of them:

- [ ] **The quiet period:** a few weeks of everyday use with no protocol or
  schema change, counted from the last level bump (level 8 on 2026-10-06;
  schema 53 on 2026-10-04).
- [ ] **Measure again on the real setup** what changed since the measurements
  of 2026-10-03 (`docs/release-measurements.md`): a tag write on the NAS with
  its undo copy, the library panel following changes on a 66,000-track
  library, and a large list checking its files.
- [ ] **Optimised builds** pass the suite again, as they did on 2026-10-03.
- [ ] **The release checklist** ([release-checklist.md](release-checklist.md))
  walked and recorded, in its engine-era form.
- [ ] **Packaging** settled: the `melody-git` recipe builds the engine
  packages under a name of the old Go daemon, and its own package is an
  empty metapackage.

## 1. Engine and clients

- [ ] **Online radio** (asked for): internet streams as something to play.
  Needs an ADR first -- where stations live (the engine, like lists), playing
  a stream with no end, stream titles (ICY), what is scrobbled, and the phone.
- [ ] The engine watching its own music folders. `melody-watch` (ADR-0232)
  tells an engine about changes on a NAS; an engine noticing them itself is
  still open.
- [ ] A terminal client (`melody-tui`). `melody-cli` covers scripting.
- [ ] Phase 5, the MPD bridge: the engine answering MPD clients. Low
  priority; nothing waits for it.
- [ ] Fuzz targets for what engines read from the network and from files:
  protocol v1, `tkq-1`, `tkfmt-1`, CUE sheets.

## 2. Lists and library views

Lists can be found, sorted, reversed, deduplicated, and edited with undo,
including a move between tabs. M3U8 import and export work. Searches can be
kept in tabs, saved, and run against any engine's library.

- [ ] Custom `tkfmt-1` columns and grouping in track views. Library tree
  views are done (ADR-0254).
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
happens there too. Ratings can be written into the files (ADRs 0237, 0245),
and a list can continue by a dynamic playlist's rule when it ends (ADR-0253).

- [ ] Calendar-relative time operators in queries.
- [ ] Continuing a list with related tracks from Last.fm. Online similarity
  needs an explicit opt-in, caching, and marking which tracks were added
  automatically.

## 7. Collection maintenance

- [ ] Integrity scans that tell decoding failures from checksum failures.
- [ ] Duplicate audio found by content, not by path or tags.
- [ ] Relinking missing files.
- [ ] Album completeness checks that show the evidence for missing tracks.

Finding two copies of a recording should help someone review them. It must
not imply that one is safe to delete.

## 8. Desktop integration

MPRIS and media keys control the engine that plays; the playing album's
cover reaches both MPRIS and notifications. Nothing is open here.

## Other recorded follow-ups

These are requirements or proposals on record, not commitments for the next
release.

- **Workspace:** a command palette, and job, diagnostic and queue-inspector
  panels.
- **Metadata and paths:** further versioned sanitization policies, richer
  typed matching, `TOTALTRACKS` when numbering, and moving companion files with
  reviewed cleanup of empty folders.
- **File undo:** offered beyond Identify albums… -- from the tag editor, and
  for a move made on its own.
- **Infrastructure:** secure credential storage, larger library, network and
  device test runs.
- **Later:** release packaging, plugins, CD ripping.

## Keeping this page

Keep current support and its limits in the feature matrix. Take an item off
this page when it is done rather than leaving it ticked. ADRs and the dated
milestone notes remain the history of how decisions were made.
