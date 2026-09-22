# ADR-0217: Grouped search starting points

Status: Accepted

## Decision

Add 25 built-in starting points to Workspace → Search, grouped as Explore,
Favourites, Listening, Audio properties, and Library maintenance. They are
separate from the user's saved searches. Parameterized presets ask for a value;
cancellation leaves the current search untouched. Applying one enables Query,
fills the existing editable query field, clears saved-search selection, and
evaluates in the selected scope. Save as stores the ordinary dialect-qualified
query, not a reference to a mutable built-in definition.

The Qt-free query module owns the catalog and deterministic tkq-1 generation.
Text values use doubled-quote escaping; numeric parameters have explicit bounds.
Decades start on a multiple of ten and include all ten years. Minimum ratings
include the boundary on the existing 1–10 scale. Listening ages are complete
24-hour periods, not calendar dates. Album matches return the matching tracks
using the existing result/list workflow.

The menu is rebuilt when opened. Remote availability uses the same capability
checks and translator as execution, including current-list search support.
Unsupported presets and empty groups are hidden; an entirely unsupported scope
gets one explanatory disabled entry. Neither stock MPD nor old Melody is given
a client-side library/history emulation. Local presets remain available without
a server connection. No Melody protocol change is necessary for this catalog.

## Bounds and exclusions

These are search starting points, not a general visual query builder. Recently
added and exhaustive lossless classification presets are deferred: the shared
query dialect does not yet expose a cross-authority added timestamp or lossless
fact. Missing ReplayGain presets explicitly describe cached tag presence, not
whether a track has been analyzed into a sidecar. Existing query limits,
cancellation, source identity and authority boundaries are unchanged.

## Related polish

Search focuses its input on first show, reshow, and reopening the existing
window. Album shuffle uses one shared stacked-album/shuffle icon in local and
Melody status controls, retaining accessible action text, tooltips, checked
state and capability gating.

## Verification

The repository-owned preset corpus records exact generated queries and rationale.
Tests compile every preset, translate every default to current Melody, reject
invalid bounds and syntax injection, and cover grouped menu availability,
parameter acceptance/cancellation, input focus, server execution and both mode
icons. Existing search and saved-search tests remain the persistence contract.

Validation: the development build and all 69 CTest suites pass, as does
`go test ./melodyd` in the sibling server repository. Changed-source formatting,
SPDX and diff checks pass. No server changes or redeployment are required.
