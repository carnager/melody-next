# ADR-0072: Native metadata transformation-chain interchange

- Status: accepted
- Date: 2026-08-31
- Owners: Trackknife project
- Extends: ADR-0049 persisted exact-value metadata transformation chains,
  ADR-0068 versioned metadata capture patterns, and ADR-0070 canonical raw
  transformation editing

## Context

Trackbench can persist, preview, and run named typed tagging scripts, but those
definitions cannot move between workspaces or be backed up independently of
the SQLite store. The Picard-style paste surface is deliberately a bounded
foreign-source translator, not a lossless representation of every Trackbench
action. ADR-0070's Raw script is likewise a canonical view over only the
representable cleanup subset.

Native interchange needs one public, lossless form for the complete current
typed chain. It must not introduce executable source, silently discard future
behavior, copy a workspace identity, or enable imported automation without
review.

## Decision

- The native form is UTF-8 JSON with the exact version-1 envelope:

  ```json
  {
    "format": "trackbench-metadata-transformation-chain",
    "format_version": 1,
    "chain": {
      "schema_version": 1,
      "name": "Prepare incoming files",
      "actions": []
    }
  }
  ```

- `format_version` versions the JSON envelope independently of the typed
  chain's `schema_version`. Version 1 accepts exactly the documented object
  keys. Unknown or missing keys, format identifiers, envelope versions, chain
  schemas, action kinds, enum values, and payload shapes fail closed. A newer
  producer must increment the appropriate version rather than relying on an
  older reader to ignore data.
- Action order and every exact-value array order are significant. JSON strings
  carry exact valid UTF-8 values, including empty values and repeated values.
  Numbers are exact non-negative 32-bit integers; fractional or out-of-range
  JSON numbers are rejected.
- Version 1 uses these stable action names and payloads in addition to `kind`:

  | `kind` | Required payload |
  | --- | --- |
  | `set_values`, `add_values` | `target_field`, ordered `values` |
  | `remove_field` | `target_field`, `match_mode` |
  | `remove_field_if` | `target_field`, `match_mode`, `dialect`, `condition` |
  | `transform_values` | `target_field`, `transform` |
  | `format_value` | `target_field`, `dialect`, `source` |
  | `copy_field` | `target_field`, `source_field` |
  | `split_values`, `join_values` | `target_field`, `separator` |
  | `remove_matching_values` | `target_field`, `match` |
  | `replace_matching_values` | `target_field`, `match`, ordered `replacement_values` |
  | `number_selected_items` | `target_field`, `start`, `padding` |
  | `keep_first_characters` | `target_field`, `character_count` |
  | `capture_values` | `dialect`, `source_kind`, `source`, `pattern` |
  | `convert_rating` | `target_field`, `source_field`, `scale` (5, 10 or 100) -- added by ADR-0244 |

- `match_mode` is `logical` or `exact_native`. `transform` is `trim_ascii`,
  `lowercase`, `uppercase`, or `capitalize_first`. `source_kind` is
  `filename`, `full_path`, `formatted`, or `field`. A dialect object contains
  exactly `dialect`, `dialect_version`, and `compiler_schema`, preserving the
  complete `tkfmt-1` or `tkcapture-1` identity.
- The envelope carries no saved-chain UUID, automatic/check state, SQLite
  details, or canonical Raw-script presentation. Import creates a new unsaved,
  unchecked editor definition. The user reviews, previews, and explicitly
  saves it; ordinary chain preview and metadata Apply boundaries remain
  unchanged.
- Import/export is limited to 8 MiB. The Qt adapter performs file I/O on a
  worker, validates through the existing Qt-free typed-chain service, and uses
  atomic `QSaveFile` replacement for export. `.tbtags.json` is the suggested
  filename suffix, not part of format identity.
- Import refuses to replace dirty editor contents without explicit discard.
  Export may capture a valid unsaved chain but does not mark it saved.

No persistence migration is required.

## Alternatives considered

### Export the SQLite action rows directly

Rejected. Database normalization, numeric action codes, saved identity, and
automatic state are workspace persistence details rather than a portable
public contract.

### Use the Picard-style or canonical Raw script

Rejected. Those forms intentionally cover only a bounded cleanup subset and
cannot losslessly represent exact multi-values, numbering, or `tkcapture-1`.

### Ignore unknown JSON keys for forward compatibility

Rejected. Re-saving such a definition could silently erase behavior. Explicit
envelope and chain versions provide truthful compatibility instead.

### Carry the saved UUID and checked state

Rejected. Cross-workspace identity collisions and unexpectedly enabled
automatic cleanup are both avoidable. The portable object is a definition,
not a catalog record.

## Consequences

- Complete current tagging scripts can be backed up and shared without
  reducing them to a foreign or partial source language.
- A native import never mutates tags, stages a draft, enters the saved catalog,
  or enables automatic execution by itself.
- The strict version-1 reader will reject benign future presentation fields;
  future writers must deliberately version and migrate the envelope.
- The public JSON adapter remains outside the Qt-free metadata core, while the
  typed chain and all behavioral validation remain toolkit-independent.

## Validation

- A dedicated interchange test round-trips every typed action, all match and
  transform modes, all capture sources, dialect identities, Unicode, ordered
  duplicates, and explicit empty values byte-deterministically.
- Tests reject invalid UTF-8, oversized input, unknown envelope versions and
  keys, unknown actions, fractional integers, and semantically invalid chains.
- Real temporary-file coverage proves atomic export bytes and exact reload.
- Offscreen editor coverage exposes native Import and Export only at their
  valid capability boundaries alongside the separate **Paste script…** action.

## Revisit when

- a second envelope or chain schema requires an explicit migration path;
- signed/trusted definition distribution becomes a real requirement;
- presentation source becomes authoritative enough to warrant a separately
  versioned optional payload.
