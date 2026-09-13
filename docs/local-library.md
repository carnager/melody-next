# Local music library

**Trackknife decision (ADRs 0115–0116):** local collection browsing is optional,
and filesystem scanning is manual.
The Library source in local context indexes folders chosen by the user. The
Folders source remains available without an import or scan. MPD retains its
own library and search.

## Using the library

1. Select a local queue or list, then choose the **Library** sidebar tab
   (ADR-0130).
2. Open **Folders…**, add one or more music folders, then press **Refresh** to
   scan them in the background. The footer shows progress; **Stop** cancels the scan.
3. Click an artist to browse albums, and an album to browse files. Enter toggles
   branches; on a track it appends the selection to the current local list.
4. Type in the search field to see separate **Albums** and **Tracks** results.
   Album matches use artist/album text; track matches also use titles. Each
   search word must match, ignoring Unicode letter case. Punctuation is literal.

Drag artists, albums, or tracks into a local queue/list's contents to copy them
at the insertion marker. Ctrl/Shift selects multiple entries. Right-click
offers **Append to current list**, **Insert next in current list**,
**Replace list and play**, **Open in new tab**, and branch **Expand/Collapse**.
Append, insert-next, and replace/play are also available as inline row buttons,
using the same controls as the MPD library. Insert next follows the playing row
in that list, otherwise the selected row or the beginning of an unselected list.
The MPD queue does not accept local library drops.

Tracks show their tagged numbers, such as **03. Title**; tracks without a number
keep their title without a numeric prefix. Album rows show embedded covers or
fall back to sibling `cover`, `folder`, or `front` JPEG/PNG files. Covers load
one at a time for visible albums and keep the record placeholder when absent.
No network lookup or library scan is triggered. Press **Refresh** after changing
folder images outside Trackbench; in-app operation notifications refresh cached
covers automatically.

Children and search results arrive in pages of 200; **Show more…** loads the
next page. Opening an artist or album resolves its complete available selection,
not only its currently visible children. Up to 1,000 entries may be selected;
requests resolving more than 100,000 file references are rejected. Overlapping
artist/album/track selections insert each path once. Unavailable entries remain visible, and
partially available selections report skipped files.

The folder list reports disconnected folders. Removing a folder forgets its
index entries without changing files, queues, or working lists. Overlapping
folders are rejected; add their common parent or separate non-overlapping roots.

## Saved searches

Open **Workspace → Search…** (**Ctrl+Shift+F**), enter a word search or enable
**Query** for a `tkq-1` expression, choose **Library database** or **Current tab**,
and press **Save as…**. Selecting its name in **Saved searches…** restores the
query, mode, and scope and evaluates it again. Current tab means the local tab
active at that moment. Library queries use the cached index without scanning.

Change the query and press **Update** to replace the selected definition;
**Rename…** changes its name and **Delete…** removes it. Names must be unique.
Saving zero-result searches is allowed. **Open results in tab** still creates an
ordinary snapshot; these tabs do not update automatically. Autoplaylists remain
a subsequent feature (ADR-0163).

## Indexing and consistency

Migration 28 adds library roots and file records to the existing SQLite store.
Paths remain raw BLOBs and are escaped losslessly for presentation. The index
is a cache of file metadata; it does not become the authority for tags.

A scan walks and commits on one thread while a bounded worker pool
(half the cores, two to eight) prepares changed files in parallel —
probe, metadata read, tag and technical extraction (ADR-0151); a
separate worker serves queries and folder configuration. The scan
compares device/inode/size/mtime revisions, prepares only changed
files, and verifies the revision again under a short per-file database
write transaction; commits may land in any order. The library
connection runs WAL with synchronous=NORMAL — a power loss can cost at
most the final commit, which the next Refresh repairs.
Only plausible audio extensions are probed; directory and file symlinks are
skipped. Scans stop after one million visited entries and report incompleteness.
Cancellation and incomplete traversal retain previously indexed entries.

A separate artwork worker shares the local-list thumbnail reader, with a
256-entry cache of 128-pixel thumbnails and missing-image results. Artwork reads
are cancelled on view changes and bounded to 16 MiB encoded input, 16 million
source pixels, and 10,000 sibling entries. Images are not stored in SQLite.

A complete scan removes confirmed missing files from the index, so deleted
albums disappear even inside an online network mount (ADR-0126). Cleanup requires
an absent path and an existing parent directory on the recorded filesystem;
uncertain paths and changed-device mountpoints retain unavailable entries.
An inaccessible root marks its cached files unavailable without deleting them;
later reconnection restores them after an explicit refresh. Scans run only when
you press **Refresh**;
startup displays cached entries, and adding folders or completing operations
does not start a scan. External changes, newly converted files, and reconnected
folders appear after the next Refresh. Only changed files need metadata probing.
Refreshing the view preserves expanded and selected entries.

Metadata and relocation commits update matching index entries in the same
transaction as persisted list/cache changes. A move between indexed roots
changes ownership; a move outside all roots removes the index entry. Failed
transactions and idempotent recovery follow the existing operation journal.
These committed changes reload the displayed index without scanning files.

MusicBrainz release IDs identify albums when present. Otherwise album artist,
album title, and parent directory identify an album. Tree labels are evaluated
off the UI thread using shipped `tkfmt-1` expressions. Opened files use the
existing declarative track-view engine, metadata reader, and local transport.

## Current limits

The index catalogs physical audio files. Chapter/subsong expansion still happens
when opening a source; separate indexed searches of those logical titles and
external cue-sheet titles are not included. Structured tkq filters (ADR-0150)
evaluate over the migration-30 field table and technical columns; index rows
written before that migration carry them only after their next explicit
Refresh. Saved searches are available in the standalone Search dialog
(ADR-0163); autoplaylists, custom library-tree expressions, and an artwork grid
remain future work.
Album cover thumbnails are available in the current tree and search results.

Device checks protect against an unmounted volume exposing a mountpoint on a
different filesystem; they cannot distinguish same-device bind-mount substitutions.

## Verification

ADR-0126 adds deletion cleanup, offline/changed-device retention, incomplete and
cancelled scan protection, multi-page/raw-path cleanup, unchanged working lists,
and cover refresh across browse/search transitions. All 58 development CTest
targets and the build/formatting checks passed on 2026-09-06. Changed-device
coverage simulates cached mount evidence without privileged mount operations.

`local-library` tests use real FLAC fixtures for background indexing and query
behavior, raw filenames, incremental refresh, offline/reconnection handling,
cancellation, root removal, tag/move transactions, migration reversal, and
opening UI search results in a local queue while preserving MPD separation.
ADR-0117 adds real-file regressions for library actions and drops, captured
destination tabs, multi-selection, unloaded pages, and raw filenames.
Its development checks passed for local-library, queue-table-view,
server-library-tree-model, and bench-main-window. The library and queue tests
also passed ASan/UBSan with leak detection; formatting and SPDX checks passed.

ADR-0118's track-number and cover tests passed alongside the development
workspace and format-probe suites. Local-library and format-probe also passed
ASan/UBSan with leak detection. Real FLAC files cover embedded/folder artwork,
raw filenames, aspect ratio, refresh/cancellation, and corrupt/oversized images.

Validated 2026-09-05: all 57 development CTest targets passed. Focused
ASan/UBSan tests for the library, list repository, and queue view passed;
the clang-tidy library/UI build and local-library test passed. The repository
formatting and SPDX checks passed. The offscreen search-and-open workflow was
also inspected visually. This does not claim a measured scan-time budget for
large collections or slow network mounts.
