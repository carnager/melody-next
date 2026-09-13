# Local music library

The local library is an optional index of folders you choose. You can still
open files through **Folders** without adding them to the library. MPD keeps
its own library on the server.

Press **Refresh** to scan for changes. Starting the app, searching, or opening
results uses the existing index and does not start a scan.

## Using the library

1. Select a local queue or list, then choose **Library** in the sidebar.
2. Open **Folders…**, add one or more music folders, then press **Refresh** to
   scan them in the background. The footer shows progress; **Stop** cancels the scan.
3. Click an artist to browse albums, and an album to browse files. Enter toggles
   branches; on a track it appends the selection to the current local list.
4. Type in the search field to see separate **Albums** and **Tracks** results.
   Album matches use artist/album text; track matches also use titles. Each
   search word must match, ignoring Unicode letter case. Punctuation is literal.

Artist rows show the number of albums on the second line. The counts load in
the background, before you expand an artist. Local and MPD library trees use
the same spacing and expansion animation.

Drag artists, albums, or tracks into a local queue/list's contents to copy them
at the insertion marker. Ctrl/Shift selects multiple entries. Right-click
offers **Append to current list**, **Insert next in current list**,
**Replace list and play**, **Open in new tab**, and branch **Expand/Collapse**.
Append, insert-next, and replace/play are also available as inline row buttons,
using the same controls as the MPD library. Insert next follows the playing row
in that list, otherwise the selected row or the beginning of an unselected list.
The MPD queue does not accept local library drops.

Right-click a local track and choose **Locate artist** or **Locate album** to
open its indexed entry in the Library sidebar. Navigation uses the exact file
path and album identity, clears the current search, and loads additional tree
pages when needed. It does not scan folders or change the track list. Files not
in the index show a message explaining how to add their folder.

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

## Searching by tags and audio properties

Enable **Query** beside the library search field to use a structured filter.
For example:

| Find | Query |
| --- | --- |
| 24-bit files | `bitspersample EQUAL 24` |
| Files missing album ReplayGain | `REPLAYGAIN_ALBUM_GAIN MISSING` |
| Files above 48 kHz | `samplerate GREATER 48000` |
| FLAC files without an album tag | `codec IS flac AND album MISSING` |

Keywords such as `EQUAL`, `MISSING`, and `AND` must be uppercase. Technical
values come from the last scan. If a query reports an incomplete index after
an upgrade, press **Refresh**, wait for it to finish, then run the query again.

Press **Enter** in the sidebar search to keep its results in a local tab. The
tab uses cached metadata; cover images load separately. Right-click a result
and choose **Edit tags…** or **ReplayGain…** when you want to work on the files.
Those operations read the files they need before preparing changes.

The [query reference](query-language.md) describes all operators and limits.

## Saved searches

Open **Workspace → Search…** (**Ctrl+Shift+F**), enter a word search or enable
**Query** for a `tkq-1` expression, choose **Library database** or **Current tab**,
and press **Save as…**. Selecting its name in **Saved searches…** restores the
query, mode, and scope and evaluates it again. Current tab means the local tab
active at that moment. Library queries use the cached index without scanning.

Change the query and press **Update** to replace the selected definition;
**Rename…** changes its name and **Delete…** removes it. Names must be unique.
A search can be saved even when it has no results. **Open results in tab**
creates a snapshot; these tabs do not update automatically. Database-search tabs
use cached tags and technicals directly, without rereading every audio file
(ADR-0164). The same applies to Enter in the sidebar search. Automatically
updating playlists are not implemented yet.

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
connection uses WAL with synchronous=NORMAL. The database is a cache;
Refresh can rebuild metadata lost after a power failure.
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
folders appear after the next Refresh. Only changed files normally need metadata probing. Schema 33 marks older field
indexes incomplete because previous limits silently dropped some tags. After
upgrading, run **Refresh** once to rebuild those records, then rerun saved
searches; existing result tabs remain snapshots. Full-field queries report an
incomplete index until Refresh finishes successfully (ADR-0166).
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

The index catalogs physical audio files. Search-result tabs retain those physical
file rows without probing or chapter/subsong expansion. Expansion still happens
when importing a source through the file-intake workflow; separate indexed searches of those logical titles and
external cue-sheet titles are not included. Structured tkq filters (ADR-0150)
evaluate over the migration-30 field table and technical columns. Schema 33
requires complete field evidence, with explicit Refresh repairing older rows. Saved searches are available in the standalone Search dialog
(ADR-0163); autoplaylists, custom library-tree expressions, and an artwork grid
remain future work.
Album cover thumbnails are available in the current tree and search results.

Device checks protect against an unmounted volume exposing a mountpoint on a
different filesystem; they cannot distinguish same-device bind-mount substitutions.

## Verification

The `local-library` tests use real FLAC files to check indexing, query results,
raw filenames, refresh, cancellation, offline folders, deletion cleanup, and
metadata/path updates. They also cover album counts, cached search tabs,
artwork, navigation through paged results, and keeping local actions out of the
MPD queue. The related `bench-main-window`, `queue-table-view`, and
`server-library-tree-model` suites check the workspace behavior.

Older test runs and sanitizer results are recorded in ADRs 0115–0118 and 0126.
These tests do not establish scan-time budgets for large collections or slow
network mounts.
