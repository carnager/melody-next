# ADR-0160: Unified tags and artwork Apply

- Status: accepted
- Date: 2026-09-13
- Supersedes: ADR-0159 separate Save and per-picture publication
- Extends: ADR-0080, ADR-0137

## Decision

**Trackbench decision:** the tag editor's main Apply includes the reviewed
artwork draft. Save tags controls text changes only; covers can be applied with
it unchecked. The editor retains explicit pending artwork review, undo selected,
discard, source-selection locking, and unsaved-change protection.

The core groups picture intents by physical file and attaches the immutable
artwork plan to the metadata source plan. Original picture edits use descending
ordinals, followed by additions; fingerprints and source revisions remain pinned
to the reviewed original. Duplicate logical occurrences share one publication.

All text and artwork changes are made in one private prepared file and verified
before a single publication. Native FLAC streams original unrelated metadata
blocks and audio unchanged, replacing only edited comments and picture blocks.
MP3 and MP4 use the qualified container writers on the private copy, then verify
text, the complete picture inventory, and container/audio preservation. The same
prepared-copy dispatcher supports metadata combined with relocation.

There is one backup and journal operation per file. Schema 31 adds a batch
artwork-evidence shape and permits text evidence in artwork records. Recovery
and undo verify both text and inventory evidence. Migration is transactional;
downgrade refuses to discard records using the new evidence shape.

This is atomic per physical file, not across the batch: cancellation stops new
work, while already published files remain committed. External images remain
donors, never deletion targets. Pending-image thumbnails and a dedicated retry
workflow remain separate improvements.

## Verification

Real FLAC, MP3/APIC, and MP4/covr regressions cover removing multiple covers and
adding a replacement alongside text changes, failed replacement preserving the
original bytes, recovery after interrupted publication, and byte-exact whole-file
undo. GUI regressions exercise the main Apply with tags enabled and disabled,
confirm that staging leaves the source unchanged, and count one committed file
and backup. Existing container, journal, library, and editor suites remain gates.
