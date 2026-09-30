# ADR-0070: Canonical raw transformation editor

- Status: accepted; its raw grammar superseded by ADR-0241 (the Raw tab is
  the native script, every step and full tkfmt-1)
- Date: 2026-08-30
- Owners: Trackknife project
- Extends: ADR-0065 pasted rule-script translation

## Context

The one-shot **Paste script…** translator made compact cleanup rules usable,
but subsequent edits required manipulating the generated typed rows. It also
made an edited-but-unsaved chain too easy to close or replace without a clear
dirty-state boundary. Compact conditional cleanup is substantially easier to
inspect and maintain as source text.

Trackbench still must not persist or execute opaque foreign code. Saved
transformation chains are versioned typed actions and their behavior cannot
depend on an external interpreter.

## Decision

- The transformation editor has **Typed rules** and **Raw script** tabs.
- Raw mode accepts the same bounded cleanup subset as ADR-0065. Every valid edit
  is immediately translated into the typed action list used by Preview, Save,
  automatic execution, and Apply. Invalid or empty source blocks Preview and
  Save with source-positioned diagnostics.
- Representable typed chains have a deterministic canonical raw projection.
  Structured edits regenerate that projection. A generated projection is
  imported again and must reproduce every typed action exactly before it is
  shown as editable.
- The initial projection covers exact-native deletion, conditional exact-native
  deletion, scalar `tkfmt-1` formatting, and keep-first-character actions.
  Other typed actions remain fully usable in **Typed rules**, while Raw mode is
  read-only and identifies the first unrepresentable step.
- Typed actions remain the persisted authority. Exact whitespace, comments,
  aliases, and spelling from pasted/raw source are not stored; reopening a
  saved chain shows its canonical projection.
- Name, typed-rule, pasted-source, and raw-source edits set the window's dirty
  state. Save is explicitly enabled for a valid changed chain, switching saved
  definitions is disabled while dirty, and Close requires explicit discard.

No schema migration is required.

## Alternatives considered

### Persist and execute opaque script source

Rejected. It would add a second mutation language, weaken action validation,
and make persisted behavior depend on compatibility with a foreign runtime.

### Keep raw source only in the paste dialog

Rejected. That does not support ordinary maintenance of compact cleanup rules
and does not expose unsaved source edits as first-class editor state.

### Serialize both source and typed actions

Rejected for this slice. Dual durable authorities can diverge. A future native
interchange format may carry presentation source only with an explicit schema
and verified typed semantic identity.

## Consequences

- Compact cleanup chains can be edited directly without losing Trackbench's
  typed preview and commit boundaries.
- Source presentation is canonical rather than byte-for-byte preserved.
- Raw coverage can grow only when a typed action has a deterministic exact
  round trip through the bounded grammar.

## Validation

- Qt-free tests export, re-import, and compare supported typed actions exactly,
  and reject an unsupported typed action.
- Offscreen UI coverage edits the conditional disc cleanup in Raw mode, sees
  the typed rows change, observes the dirty marker and Close protection, then
  saves through the ordinary typed-chain store.

## Revisit when

- native transformation-chain interchange defines a public serialized form;
- comments or stable user formatting are important enough to justify a
  separately versioned, non-authoritative presentation-source field.
