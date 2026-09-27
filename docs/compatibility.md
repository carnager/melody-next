# Compatibility and inspiration

Updated on 2026-09-27. Trackknife is one application, a client of engines
(`melodyd`, ADR-0220): each engine owns its library, lists and playback, and
Trackknife, the phone and `melody-cli` talk to it over protocol v1. The
two-authority model -- an MPD side and a local side in one window, ADR-0058 --
is retired, and with it the MPD client (ADR-0224).

## Protocols

**Trackknife decision:** the engines' own protocol, v1 (ADR-0222, ADR-0223),
is the only one Trackknife and its clients speak. It is JSON lines over a unix
socket or TCP; a TCP peer must give a password. It is versioned by the
`protocol` number in `engine.info`, and a field added to a document is ignored
by a client that does not know it, so older clients keep working (ADR-0236's
`sequence` is such a field).

**MPD** is not a compatibility target of Trackknife any more: it does not
speak MPD, and a library that used to be an MPD server is now an engine on
that machine. The opposite direction -- an engine answering MPD clients such
as `mpc` or Cantata -- is Phase 5 of the engine plan, the MPD bridge. It has
not started and nothing waits for it. **Proposal:** if it is built, it targets
the documented MPD protocol and must work with stock MPD clients; MPD song ids
and queue versions are derived from the engine's entry identities, not the
other way round.

## Product relationship to foobar2000

Trackknife is a spiritual successor to foobar2000, not a clone. It keeps what
makes foobar2000 valuable: tabbed lists, fast interaction, gapless playback,
ReplayGain, broad format support, powerful metadata work, configurable views,
conversion, and predictable bulk operations. It does not reproduce
foobar2000's interface, component ABI, private configuration formats, or
scripting quirks.

## Formatting language: no external compatibility promise

ADR-0008 replaces the former 1:1 foobar2000 title-formatting target. The
`tkfmt-1` language uses the familiar `%field%` and `$function(arguments)`
shape also used by MusicBrainz Picard, but the project's own specification
([title-formatting.md](title-formatting.md)) and its test corpus are
normative. The same language serves Trackknife's views, the engine
(`playback.format`, `catalogue.find`), `melody-cli --format`, and conversion
and file naming.

Consequences:

- foobar2000 and Picard scripts may look similar but are not promised to run;
- there is no separate foobar truth flag or optional-section behaviour;
- Picard's metadata-changing functions such as `$set` are intentionally
  absent; ADR-0065's paste importer may translate a documented subset into
  ordinary Trackknife rules without running Picard code;
- missing fields, escaping, integer conversion, multi-values and every
  built-in follow `title-formatting.md`;
- an importer or compatibility dialect stays separate and must not change
  `tkfmt-1`.

## Search and query syntax

Foobar-style query syntax is not a compatibility requirement. Queries are
`tkq-1` (ADR-0150): Trackknife's own versioned dialect with a foobar-inspired
keyword surface, specified in [query-language.md](query-language.md). The
engine evaluates them, for every client. Persisted queries follow the same
contract as formatting expressions: exact source, dialect, dialect version and
compiler schema. A query may embed `tkfmt-1`, for sort keys
(`SORT BY $num(%tracknumber%,2)`) and expression predicates.

## Metadata and workflow parity

Trackknife needs equivalent outcomes for mass editing, arbitrary and
multi-value tags, derived values, safe file operations, ReplayGain,
conversion, library views, lists and integrity checks. Equivalent outcomes do
not require matching dialog layouts, menu locations, preset file formats or
implementation details.

## Deliberately unsupported

- foobar2000 component binaries or SDK ABI;
- Default UI/Columns UI layouts, themes and colour-control strings;
- proprietary configuration databases, and `.fpl` as native storage;
- Windows-only path, shell, output or codec behaviour;
- undocumented title-formatting and query quirks;
- Picard plugin APIs and metadata-changing tagging scripts;
- acting as an MPD client.

The ADR-0065 paste importer helps migrate a small documented cleanup subset by
translating it into Trackknife's versioned models. It does not make the
external language or source text canonical. Per ADR-0066, imported deletion
targets the exact native field name; Trackknife does not read a legacy or
custom spelling as an alias of a conventional field.

## Identity: an entry is not a path

A list needs more than a path. ADR-0221 gives every list entry two identities
and never lets one stand in for the other:

- **Entry identity** -- which slot in this list. Assigned when the entry is
  added, kept through reordering, gone when it is removed. It tells the same
  track queued twice apart, and it is what the engine's queue, Up Next and
  resume name.
- **Track identity** -- which piece of music. A hash over a fixed set of
  metadata fields; it survives a re-encode or a move, and ratings and history
  hang on it.

A path is data, not identity. When a move or rename succeeds, the library,
lists, queue and statistics follow it as one logical change, on every engine
that holds the file (`list.relocate`, ADR-0233).

Metadata precedence is:

1. freshly read embedded or container data;
2. explicit Trackknife sidecar overrides (`.tkmeta`);
3. the engine's current library record;
4. what a list entry carries -- the tags whoever queued it gave -- only while
   the file is unavailable or unread.

Reconciliation compares identity and revision before replacing cached data,
and the UI shows when it is presenting stale or fallback data.

## Versioned persistence

Persist every formatting expression with:

- dialect and dialect version (`tkfmt`, `1`);
- exact original source;
- compiler-schema version;
- typed usage context;
- optional human name.

Never persist only a compiled AST. Parsers change; users must keep editable
source and stable behaviour. The same rule holds for `tkq-1` queries.
