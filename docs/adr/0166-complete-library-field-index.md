# ADR-0166: Complete field evidence for library queries

- Status: accepted
- Date: 2026-09-13
- Supersedes: ADR-0150 per-field index truncation

## Problem

The former 64-name, 32-values-per-name, and 2048-byte-value index limits silently
omitted readable metadata. A file with many alphabetically earlier tags could
lose its ReplayGain fields in the index, making `REPLAYGAIN_ALBUM_GAIN MISSING`
return a false match even though Edit tags showed the native field.

## Decision

**Trackbench decision:** index every nonempty readable value within overall
bounds: 4096 canonical field names, 16384 values, and 4 MiB of field-name/value
text per file. Exceeding a bound is an explicit error, never partial success.
These follow the native metadata reader's existing scale of limits. Empty values
retain the query dialect's existing absent-value semantics.

Schema 33 adds a complete-field marker and a partial index covering available
incomplete records. Full-field query evaluation checks this marker before
returning results. An available incomplete record produces a visible request to
Refresh rather than interpreting unknown fields as missing. Ordinary artist/album
browsing and its denormalized word-search data remain available.

Existing records start incomplete because the old index cannot prove whether
values were discarded. Only explicit Library Refresh scans files; incomplete
records are reindexed even when their filesystem revision is unchanged. Fresh
field rows and the complete marker commit together. A metadata-read failure
cannot fall back to a partial stream-tag projection and claim completeness;
existing failed records are marked incomplete. Sources unsupported by the native
metadata reader retain the existing stream-tag fallback.

Verified in-app metadata publication writes complete fields transactionally.
Path-only moves preserve the marker with the retained fields. Development
downgrade removes only the marker/index, preserving field rows; upgrading again
conservatively requires Refresh. Existing search-result tabs remain snapshots
and must be rerun after Refresh.

## Verification

Real FLAC regression fixtures have 90 extra field names before ReplayGain,
40 genre values, and a 4096-byte text value. Presence/value queries retain these
fields and the missing-gain query returns only the genuinely untagged file.
Downgrade/upgrade refuses to return full-field query results before explicit
Refresh, which repairs unchanged files once; the following Refresh skips them.
Migration round-trip and repository version checks include schema 33.
