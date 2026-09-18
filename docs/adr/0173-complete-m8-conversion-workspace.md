# ADR-0173: Complete the M8 conversion workspace

- Status: accepted
- Date: 2026-09-16
- Owners: Trackknife project
- Extends: ADRs 0105-0111, 0131-0134, 0154, and 0155

## Context

The qualified converter already supplied four encoders, exact logical-range
decode, bounded parallel scheduling, resampling and bit-depth policy, metadata
and one resolved cover, collision preview, full output decode verification,
and atomic no-replace publication. M8 still lacked explicit channel and
permanent-gain controls, and a saved encoder preset did not restore the rest of
the job even though the milestone requires reproducible presets.

The early M8 list also proposed grouped multi-track and merged outputs. Those
modes conflict with the established one-row/one-output naming and metadata
model: they need a separate chapter/CUE metadata design and cannot honestly be
treated as another encoder toggle.

## Decision

### Complete signal policy

Conversion exposes Keep source, Mono, and Stereo channel policies. FFmpeg's
channel-layout-aware resampler performs the requested mix before encoding.
It also exposes no permanent gain, Track ReplayGain, and Album ReplayGain;
Album falls back to Track. Effective metadata values override physical decoder
tags, matching logical-source provenance. Gain is applied to floating-point
PCM before channel/rate/bit-depth conversion, and a matching stored peak caps
amplification at full scale. Every conversion still strips and verifies the
absence of stale ReplayGain/R128 output fields.

This is the bounded built-in DSP surface for M8. An arbitrary ordered DSP graph
belongs to later playback/conversion expansion; Trackbench is not a mastering
suite.

### Saved presets reproduce the whole interactive job

Creating a saved encoder preset also stores a schema-1 job snapshot keyed by
its stable preset identity: exact runtime FFmpeg/libavcodec/libavformat/
libswresample versions, destination root, naming expressions, mirror mode,
sample-rate and bit-depth policies, channel and gain policies, artwork mapping,
and concurrency cap. Selecting the saved preset restores every value; deleting
it removes the snapshot. The immutable built-ins remain encoder starting
points and the ordinary last-used settings remain independent.

### Output-mode boundary

M8 qualifies one verified output per selected logical track. CUE segments,
container chapters, and tracker subsongs already fan out through their exact
selection/range and therefore satisfy cue-aware conversion. Grouped chapter
containers and merge-all output are deferred until they have a typed metadata,
boundary, and naming contract; they are not silently approximated. Multiple
artwork carriage and automatic output loudness scans are likewise optional
extensions: one resolved cover is verified byte-exactly, and removal of stale
loudness satisfies the output-loudness safety gate.

ADR-0154 remains the mirror contract: `/` is the declared source root, so the
complete absolute source hierarchy is recreated below the chosen destination.
Expression mode is the explicit way to trim that hierarchy. All publication is
plain filesystem work; an MPD music directory adds no protocol dependency.

## Exit-criterion reconciliation

- Saved presets restore codec, processing, naming, mapping, and concurrency,
  and record exact backend versions.
- Mirror paths are deterministic relative to `/`, visibly previewed, and the
  planner rejects lexical escape/collision.
- Hidden sibling temporaries are completely decoded and verified before an
  atomic no-replace publish; failure and cancellation remove only temporaries.
- Every conversion removes and verifies absence of stale ReplayGain/R128 data.
- A 1-16 pull pool (interactive default four) runs outside the UI and playback
  real-time threads with one decode/encode pipeline per worker.
- Destination handling uses only local filesystem APIs, including when the
  chosen directory is also an MPD music root.

M8 is complete. Grouped/merged outputs, a general DSP graph, multiple-picture
mapping, and automatic post-conversion scanning remain independently scoped
enhancements and do not weaken the qualified one-output workflow.

