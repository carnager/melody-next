# ADR-0212: One-shot album-preserving list shuffle

Status: Accepted

## Decision

Ship an explicit **Shuffle albums** command before adding a continuous
album-random playback mode. It reorders the whole local list or Melody's active
unnamed queue, not the selection. It does not toggle Random or start playback.
With Random off, subsequent normal traversal follows the resulting order.
The currently playing occurrence and its offset remain unchanged.

Group by exact cached album artist (artist fallback), album title, and date,
matching the default local album presentation. Matching non-contiguous rows
become one group. Empty album names are independent singleton occurrences.
Do not merge editions with different dates or artists. Within each album,
preserve existing list order, including deliberate duplicates; do not silently
sort by track number. MusicBrainz-ID-aware grouping remains a separate extension.

The local pure planner takes an explicit random seed. The UI generates a fresh
seed and reuses the cancellable, bounded snapshot/planning worker, progress bar,
stale-result rejection, persistent-index remapping, and one-step undo from
ADR-0127. Existing one-million-row limits apply; grouping keys are bounded to
64 KiB per row and 64 MiB total.

Melody advertises `melody_shuffle_albums REVISION`. The server reads cached album
metadata in batches outside the queue lock, then verifies the revision and
occurrence IDs before applying a complete permutation. Its queue limit is 10,000
entries and metadata reads have a three-second timeout. IDs and priorities follow
their occurrences; active and pending Up Next occurrences remain in their slots,
and their FIFO and captured return candidates remain untouched. Only the next
preload is refreshed, never the current source. Persistence errors report that
the queue changed but could not be saved; clients never retry automatically.

Trackbench exposes the server command only when advertised and the visible MPD
Queue is the active unnamed context. It must not shuffle a different active
named playlist while displaying the stashed unnamed queue. Named/inactive server
lists are not supported in this increment. Stock MPD has no emulated version.
Server queue reordering has no client-owned undo, matching existing server edits.

## Verification and remaining work

Seeded tests cover group contiguity, internal order, duplicate occurrences,
unknown albums, distinct dates, cancellation, local undo and playing-index
preservation. Protocol tests check the revision argument. UI tests cover local,
capable Melody, stock MPD, and a different active server context. Melody tests
cover active request/FIFO preservation, playing duplicate identity and elapsed
time, and stale-revision rejection.

Continuous album-random playback, named server-list shuffle, MusicBrainz-aware
release grouping, and history query/sort fields remain open. This increment does
not complete the broader listening-history/album-playback priority.
