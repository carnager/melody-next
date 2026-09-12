# ADR-0153: Search dialog and probe-technicals on rows

Date: 2026-09-12

Status: accepted

## Context

Search lives only inside the library sidebar: the tkq query mode
(ADR-0150) and the committed result tabs (ADR-0140) all hang off the
panel's search field, and nothing can search the rows of the tab the
user is actually looking at. Files dumped into a tab — from the file
explorer, a playlist import, a committed search — flow through the
probe pipeline for metadata, but the probe's technical facts are
discarded at the row boundary, which is exactly the gap ADR-0142
recorded when the Find bar could not match codec or sample rate.

Two requests drive this: a standalone search dialog that can target
either the library database or the current tab, with selectable
results, context actions, and an "open results in a tab" button; and
detail scanning at dump time so tab-scoped searches can answer
technical predicates at all.

## Decision

### Rows retain probe technicals

`LocalTrackRow` gains an optional technicals block — codec, sample
rate, bits per sample (via the shared `bits_per_sample_hint`),
channels, bitrate — filled by every probe-driven row builder
(whole-file, chapter, subsong, CUE) from the probe the ingest pipeline
already runs. Dumping files into a tab therefore scans everything a
search needs, in the same bounded background pass that reads their
tags today. The Find bar's haystack includes the retained values, so
"flac" or "44100" match rows the way MPD rows already match their
`audio_format` — closing the ADR-0142 follow-up for probed rows.

Rows restored from a saved workspace keep their persisted metadata and
are not re-probed at startup; their technicals stay absent until
something asks for them. That is deliberate: startup must not churn a
network library, and the dialog below fills the gap on demand.

### Row-level tkq evaluation becomes public

The planner's candidate evaluation moves behind a public persistence
API: a row-facts structure (canonical multi-value fields with original
and normalized spellings, technical values, denormalized search text)
built from a `MetadataDocument` plus optional technicals, an exact
`tkq_matches` predicate over it, and a sort-key evaluator for the
query's SORT clause. `LocalLibrary::filter` keeps its SQL pushdown;
the in-memory semantics are now one shared implementation instead of
a private copy.

### The dialog

A non-modal search dialog (menu entry and shortcut, one instance)
mirrors the panel's search semantics: the same word-search default,
the same Query checkbox for tkq, the same inline diagnostics, the same
debounce. A scope selector chooses:

- **Library database** — compiles and runs through
  `LocalLibrary::filter`, paged like the panel.
- **Current tab** — evaluates the compiled query over the active local
  tab's rows via the shared row evaluation, honoring the SORT clause.
  When the query references technical pseudo-fields and some rows lack
  technicals, the dialog first probes exactly those rows — bounded,
  cancellable, with visible progress — and stores the results back
  onto the rows so the next search is instant. Queries that never
  touch technicals evaluate without any probing. The scope is
  disabled while an MPD tab is active.

Results list selectable rows with the search presentation format;
the context menu offers the standard destinations (append, play next,
replace, new tab), and an "Open results in tab" button keeps the
whole result as an ordinary scratch tab: database results resolve
their paths through the existing discovery pipeline (the ADR-0140
snapshot path), tab results copy the matched rows directly.

## Consequences

- Tab-scoped technical queries are exact for everything dumped from
  now on and self-repairing for older tabs the first time such a
  query runs against them.
- Technicals are not persisted with list snapshots; a restored tab
  pays one on-demand probe pass per session at most, and only when a
  technical query actually targets it. Persisting them is a possible
  later migration if that pass proves annoying.
- The dialog owns no new search language or storage — it is a second
  surface over tkq-1 and the same result-tab semantics, which keeps
  saved searches (the next Area 2 package) applicable to both
  surfaces unchanged.
