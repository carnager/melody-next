# ADR-0156: ReplayGain from the track context menu

Date: 2026-09-12

Status: accepted

## Context

Scanning ReplayGain requires opening the full Properties workspace,
finding the ReplayGain section in its side panel, scanning, and
applying — a lot of surface for the most routine loudness task there
is. The live request asks for the foobar2000 shape: a top-level
"ReplayGain…" entry in the track context menu that opens a small
dialog with the scan options and runs the job.

The workspace trust principle (ADR-0083) applies: no routine review
detours. Properties remains the review-first surface; the context
path is scan-and-write.

## Decision

The local track context menu gains "ReplayGain…" beside Properties
and Convert. It opens a compact dialog for the current selection:

- The three persisted options — grouping mode (including disc-merged
  and `tkfmt-1` expression), "Store in sidecar only", "True peak as
  ReplayGain peak" — bound to the same QSettings keys Properties
  uses, so both surfaces always agree.
- "Scan & apply" captures the selection exactly like Properties does
  (same source capture, including CUE/chapter/subsong identities),
  runs the same bounded loudness scan with progress and cancel, turns
  the measurements into the same proposals (R128 for Opus, ADR-0149;
  peak policy, ADR-0148), and writes them immediately through the
  identical journaled write-plan pipeline — sidecar routing,
  unwritable-format fallback, recovery, and undo history included.
- Problems (unmeasurable files, failed measurements, blocked writes)
  land in a bounded problems pane; the status line summarizes counts.
  There is no draft grid: review-first workflows keep using
  Properties, which the dialog links to by name.

## Consequences

- The routine case is two clicks from any track list, and the two
  surfaces cannot drift because they share options storage, scan
  engine, proposal rules, and the write pipeline.
- Scan-and-write means mistakes cost an undo, not a review pass —
  consistent with the workspace trust principle and covered by the
  existing operation history.
- The dialog is selection-scoped; whole-library runs remain a
  Properties/tkq workflow.
