# ADR-0242: The language references ship inside Trackknife

## Status

Accepted, 2026-09-30.

## Context

tkfmt-1 is typed in several places -- tagging scripts, sorting, the list edit
bar, layouts -- and since ADR-0241 a whole tagging script can be written as
text. Its only description lived in the source tree (`docs/`), where a user
of a package never sees it.

## Decision

- `docs/title-formatting.md` (tkfmt-1) and `docs/tagging-scripts.md` are
  compiled into the application as Qt resources. There is no installed file
  to find and none that can be out of step with the binary.
- A **Help** menu in both windows opens **tkfmt-1 reference** and **Tagging
  script reference**; the tagging-script editor's Raw script tab has both as
  buttons.
- Opening one writes both as web pages into the cache directory -- Qt's own
  Markdown conversion, a small stylesheet that follows the browser's light or
  dark -- and hands the page to the desktop's browser. Each links to the
  other; links into the rest of the source tree are reduced to their text.

## Consequences

- The references are the same text the project maintains, so updating the
  documentation updates the help.
- The tkfmt-1 page is the specification, written for precision more than as
  a tutorial; a gentler guide can be added beside it.

## Validation

`tests/language_reference_test.cpp` writes both pages into a temporary
directory -- no browser is started -- and checks their titles, styling,
tables, cross-links and that no source-tree link survives.
