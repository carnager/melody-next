# ADR-0172: Close M7 universal ReplayGain

- Status: accepted
- Date: 2026-09-16
- Owners: Trackknife project
- Extends: ADRs 0097-0100, 0119, 0124, 0138-0149, and 0156

## Context

M7's measurement, review, persistence, and playback slices landed over several
milestones. The remaining `Partial` labels no longer described missing M7
storage coverage: qualified whole-file formats use their interoperable native
mapping, CUE tracks use `REM REPLAYGAIN_*`, and every other logical or
unwritable source uses the versioned loudness sidecar. Playback likewise has
the promised source modes, preamps, exact bypass, and peak-based clipping
prevention.

The gate still needed one repository-wide proof that decode support implies
scan support, and an explicit boundary between the shipped playback policy and
possible extra processing controls.

## Decision

### Universal scan coverage is a regression gate

The loudness analyzer test materializes every decodable audio fixture in the
repository and scans the complete corpus through the real bounded FFmpeg and
libebur128 graph with true-peak measurement enabled. The corpus includes the
qualified writable formats, read-only containers, chapters, and tracker
subsongs. A fixture may be too short for a gated loudness number, but opening,
decoding, revision capture, and analysis must succeed independently of tag
writability.

### The M7 resource policy stays deliberately conservative

The scan API remains a caller-selected pool of 1-16 workers with one decoder
per worker. Interactive callers use the default of two workers, and the scan
runs outside the UI thread. Playback has its own producer and lock-free
real-time consumer; metadata publication uses separate bounded pools. M7 does
not add a global scheduler or automatically consume every hardware thread.
Those are cross-operation hardening concerns, not a reason to remove the
bounded parallel scanner.

### The shipped playback-processing policy is complete for M7

Off is a sample-exact bypass. Track and Album apply their selected gain plus
the with-data preamp; Album falls back to Track. Automatic chooses Album for
ordered playback and Track for Random. When a matching stored peak exists,
positive amplification is reduced to keep the predicted peak at or below
full scale. The persisted scan choice determines whether that stored value is
the sample peak or oversampled true peak. Sources without usable gain receive
only the without-data preamp.

Separate user-selectable modes for applying gain without peak protection or
for peak-limiting the without-data preamp are future DSP preferences. They are
not needed to satisfy M7's gain-mode, preamp, bypass, and clipping-prevention
contract. Embedded tags and CUE lines cannot label peak kind interoperably;
the sidecar and CSV retain that provenance, and the UI must not infer a kind
where the carrier cannot express it.

## Exit-criterion reconciliation

- Every decodable repository audio fixture traverses the real scan graph.
- Album loudness reduces retained libebur128 states as one programme and has
  unequal-duration regression coverage; it is never an average of gains.
- Scan revisions are checked after decode and again by the previewed commit
  path, so changed input cannot receive an old result.
- Real-container writer tests cover the qualified standard mappings; CUE and
  sidecar tests cover parse/write/undo/provenance and stale-source rejection.
- The pull pool is bounded and parallel, defaults to two interactive workers,
  and cannot execute decode, file I/O, or allocation on the playback callback
  or UI thread.

M7 is complete. Converter output rescanning, configurable DSP chains, and a
shared resource-class scheduler remain M8 or hardening work and do not reopen
this gate.

