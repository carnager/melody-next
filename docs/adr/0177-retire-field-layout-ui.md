# ADR-0177: Retire field-layout controls from Properties

## Status

Accepted; supersedes ADR-0170's user-facing field-layout controls.

## Decision

Properties always presents the complete field inventory, with its existing text
filter and Changed fields only switch. The ambiguous All fields, Save layout,
and Remove layout controls are removed. Existing persisted layout data is left
untouched but ignored, so upgrading never deletes user state.

The useful collection-cleanup operation is instead a previewed metadata
transformation: remove named fields (blacklist) or remove everything except
named fields (allowlist). That belongs in the Scripts panel and must use the
ordinary draft preview before Apply; it is follow-up work rather than a hidden
mutation in a display filter.

The selection-consistency Suggest command moves under More because it is a
specialized draft generator, not a primary editing action.
