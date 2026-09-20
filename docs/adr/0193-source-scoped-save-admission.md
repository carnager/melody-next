# ADR-0193: Scope metadata save admission to the destination source

Status: Accepted (2026-09-20)

## Context

An existing workspace contained a `needs_reconciliation` journal for an unrelated
NAS file with missing occurrence and change rows. Loading all incomplete journals
before each save made every newly imported local track fail with inconsistent
recovery evidence, including cover-only changes.

## Decision

**Trackknife decision:** SQLite save admission selects incomplete records by the
exact raw source path before loading and validating their recovery evidence.
Both media and folder-image publication use this source-scoped query. A damaged
record for that source still rejects the save. Other sources can proceed.
The interface fallback retains conservative full validation for other journal
implementations. Whole-workspace recovery continues to report malformed evidence;
this change neither deletes records nor invents missing recovery details.

## Verification

A real-file regression reproduces missing journal children in an existing SQLite
workspace. It verifies that the damaged source remains blocked and recovery still
reports the problem, while another FLAC successfully receives an embedded front
cover and a journaled folder image.
