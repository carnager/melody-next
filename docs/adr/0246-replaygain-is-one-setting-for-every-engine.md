# ADR-0246: ReplayGain is one setting, for every engine the window reaches

## Status

Accepted, 2026-10-01.

## Context

ReplayGain was told only to the engine playing. With two engines -- this
computer's and a NAS -- they drifted apart: one on album gain, the other off.
And the setting was read back at start only by the widgets window: the Qt
Quick window started "off", showed "off", and sent "off" with the next mode it
changed, though the engine playing was on album gain.

## Decision

- The playback modes and ReplayGain, as the user left them, are read by the
  workspace when it is made -- for either window.
- ReplayGain is one setting: every connected engine is given it when it
  changes, and each engine when it connects. "Auto" stays the window's policy,
  resolved against its own shuffle before it is sent.
- A change made on the playing engine by another client is taken, saved, and
  given to the other engines. Only a change counts: what an engine reports
  before it has been given the setting is its default, and a report from
  before the window's own change, still on its way, is neither.
- Repeat, shuffle and the other modes stay the playing engine's alone: they
  decide what one queue plays next.

## Validation

`tests/engine_playback_test.cpp` (`modesAndReplayGainReachTheEngine`), with the
engine pushing state as melodyd does: a saved "album" reaches it untouched --
its own "off" arriving first does not undo that -- a bare workspace reads it,
and a change made through another client's connection is taken by the window.
