# ADR-0178: Previewed field-filter tagging scripts

## Status

Accepted, 2026-09-16.

## Context

ADR-0177 retired field layouts because they only hid rows and made the tag
editor harder to understand. Users still need a quick way to remove unwanted
metadata from a collection. That operation changes files, so it belongs in the
existing Scripts plan/preview/draft/Apply workflow rather than in presentation
settings.

## Decision

Tagging scripts provide two typed actions:

- **Remove listed fields (blocklist)** removes the named logical fields.
- **Keep only listed fields (allowlist)** removes every logical field not named.

Names use the transformation engine's case-insensitive canonical field
identity. Each action stores a non-empty bounded ordered list. The planner
expands the action against the complete selected field inventory, shows every
removal in the ordinary preview, and stages nothing until **Add to draft**.
Apply therefore retains the existing conflict detection, journaling, recovery,
and undo behavior.

Saved scripts persist the typed lists in database schema 34 and native JSON
interchange. Raw cleanup-script syntax does not gain an implicit equivalent;
the structured step remains authoritative and raw mode reports that the action
is not representable.

## Consequences

An allowlist remains reusable when a later selection contains fields that were
not present when the script was created. This is intentionally different from
expanding it into a fixed sequence of single-field removals. Display filtering
continues to be independent and never changes Apply scope.
