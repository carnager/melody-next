# ADR-0164: Open library search results from cached rows

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0140, ADR-0153, ADR-0163

## Decision

**Trackbench decision:** opening database search results in a tab uses the
indexed metadata and technical information. It must not send the result paths
through file discovery, metadata readers, or audio probing. This applies to the
standalone Search dialog and committed sidebar searches in both query and word
modes. Selection order, query sorting, and full-result opening are preserved.

A Qt-free repository API resolves up to 100,000 exact raw paths into cached
row facts in one read transaction, preserving order and duplicates. Reads are
cancellable and execute on the existing search workers. Missing index records
report a stale result instead of falling back to filesystem discovery. No schema
change or new library scan is required.

The GUI projects these facts into already-enriched list rows. Metadata carries
cached-snapshot provenance; native spellings, unsupported objects, file revisions,
and absent technical properties are not fabricated. The index's existing field
limits still apply, and these documents are presentation snapshots, never a
complete native write baseline. Playback and Edit tags retain their existing
on-demand file access and fresh validation boundaries. Search insertion also
schedules the existing independent artwork loader: cached covers are reused,
and missing covers are read asynchronously once per album without tag probing.

Search results remain physical-file snapshots. Chapter/subsong expansion belongs
to explicit file intake until logical titles are indexed. Opening a search does
not silently expand a file into different tracks from the ones that matched.

Album row geometry batches header-size changes with blocked signals. Each batch
must explicitly recompute the table's scroll geometry so the scrollbar includes
all album-header height, including after switching back to an ungrouped view.

## Verification

Real-FLAC tests index tags and technicals, move the source directory out of reach,
and open cached results through the standalone dialog and sidebar-to-tab path.
They cover raw path bytes, order, duplicates, cancellation, missing cache entries,
query sorting, metadata provenance, duration, and retained technicals. A grouped
2,000-row view regression proves the last row remains visible after keyboard
navigation and that disabling grouping shrinks the scrollbar range. The scrolling
regression failed before the explicit geometry update.

Sidebar and standalone search-tab regressions also verify cover loading from a
cold artwork cache while row metadata retains cached-snapshot provenance.
