# ADR-0159: Staged bulk artwork editing

- Status: accepted
- Date: 2026-09-13
- Extends: ADR-0078, ADR-0080, ADR-0137

## Context

The artwork editor accepted several selected pictures but its planner allowed
only one change per physical file. Selecting two versions of the same cover
therefore failed. Selecting an external image alongside embedded pictures also
disabled Remove. Immediate writes made removing covers and choosing a new
image unnecessarily risky and difficult to review.

## Decision

**Trackbench decision:** Remove, Replace, Add image, Copy, and archive fetching
stage artwork changes. A visible pending table describes each physical change;
removed inventory rows are struck through. Save artwork validates the entire
pending set and explicitly commits it. Undo selected removes individual pending
changes; Discard changes drops the whole draft without writing. Ctrl+A selects the inventory and Delete stages removal. Read-only and external
rows do not prevent edits to selected writable embedded pictures. External
images remain shared donor files, never implicitly deleted from disk.

The file selection is fixed while artwork changes are pending or saving. Tag
Apply waits for artwork Save/Discard; closing (including Escape) warns about
unsaved artwork.
Successful artwork commits advance the pending tag draft through each published
revision without clearing its text edits or undo history.

The planner identifies existing pictures by raw path, original ordinal, and
fingerprint, independently of role. Identical repeated queue occurrences
collapse; contradictory changes to the same picture remain blockers. Different
pictures in a file are independent reviewed steps. Additions are distinct by
image and role; duplicate encoded image additions are blocked before writing.
A fetched front replaces all embedded front variants with one addition,
preserving other roles. It also supersedes pending front additions.

Steps execute serially within a file, from descending original ordinal to
ascending additions. Different files use the bounded existing worker pool.
Only a successful commit's published revision may advance the next step's
expected revision; external changes still fail closed. A failure stops subsequent
steps for that file. Embedded copy donors with pending writes are blocked before
execution: exporting the donor first supplies an independent image.

Each step uses the existing qualified prepared-copy writer and recovery journal.
This is recoverable per picture, **not an atomic transaction across all pictures
or files**. Cancellation and failure can leave earlier steps committed. Feedback
counts changes and explicitly describes partial updates; the refreshed inventory
is authoritative before retrying. No journal migration or weaker preservation
proof is introduced. Multiple steps also mean multiple file copies and retained
backups; a future per-file composed writer can improve this without changing the
staged interaction.

## Verification

Real-file regressions cover multiple front images in FLAC, MP3/APIC, and
MP4/covr, repeated logical occurrences, remove-and-readd of the same image,
ordinal/revision sequencing, stale-plan rejection, and text preservation.
Workspace regressions cover selecting all with external images, several pictures
per file, unchanged files before Save, discard, explicit Save, refreshed inventory,
archive choices, and preservation of pending tag edits.
